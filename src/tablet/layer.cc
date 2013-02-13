// Copyright (c) 2012, Cloudera, inc.

#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <glog/logging.h>
#include <tr1/memory>
#include <vector>

#include "common/generic_iterators.h"
#include "common/iterator.h"
#include "common/schema.h"
#include "cfile/bloomfile.h"
#include "cfile/cfile.h"
#include "gutil/strings/numbers.h"
#include "gutil/strings/strip.h"
#include "tablet/layer.h"
#include "util/env.h"
#include "util/env_util.h"
#include "util/status.h"

namespace kudu { namespace tablet {

using cfile::CFileReader;
using cfile::ReaderOptions;
using std::auto_ptr;
using std::string;
using std::tr1::shared_ptr;

const char *Layer::kDeltaPrefix = "delta_";
const char *Layer::kColumnPrefix = "col_";
const char *Layer::kBloomFileName = "bloom";
const char *Layer::kTmpLayerSuffix = ".tmp";

// Return the path at which the given column's cfile
// is stored within the layer directory.
string Layer::GetColumnPath(const string &dir,
                            int col_idx) {
  return dir + "/" + kColumnPrefix +
    boost::lexical_cast<string>(col_idx);
}

// Return the path at which the given delta file
// is stored within the layer directory.
string Layer::GetDeltaPath(const string &dir,
                           int delta_idx) {
  return dir + "/" + kDeltaPrefix +
    boost::lexical_cast<string>(delta_idx);
}

// Return the path at which the bloom filter
// is stored within the layer directory.
string Layer::GetBloomPath(const string &dir) {
  return dir + "/" + kBloomFileName;
}

// Utility class for pulling batches of rows from an iterator, storing
// the resulting data in local scope.
//
// This is probably useful more generally, but for now just lives here
// for use within the various Flush calls.
class ScopedBatchReader {
public:
  ScopedBatchReader(RowIteratorInterface *src_iter,
                    bool need_arena) :
    iter_(src_iter)
  {
    if (need_arena) {
      arena_.reset(new Arena(16*1024, 256*1024));
    }

    int buf_size = sizeof(buf_);
    batch_size_ = buf_size / iter_->schema().byte_size();

    // TODO: handle big rows
    CHECK_GE(batch_size_, 1) << "could not fit a row from schema: "
                             << src_iter->schema().ToString()
                             << " in " << buf_size << " bytes";

    block_.reset(new RowBlock(iter_->schema(), &buf_[0],
                              batch_size_, arena_.get()));
  }

  bool HasNext() {
    return iter_->HasNext();
  }

  Status NextBatch(size_t *nrows) {
    if (arena_ != NULL) arena_->Reset();

    *nrows = batch_size_;
    return iter_->CopyNextRows(nrows, block_.get());
  }

  void *col_ptr(size_t col_idx) {
    return &buf_[iter_->schema().column_offset(col_idx)];
  }

  uint8_t *row_ptr(size_t row_idx) {
    return block_->row_ptr(row_idx);
  }

private:
  RowIteratorInterface *iter_;
  scoped_ptr<Arena> arena_;
  scoped_ptr<RowBlock> block_;

  // Use a small buffer here -- otherwise we end up with larger-than-requested
  // blocks in the CFile. This could be considered a bug in the CFile writer.
  // If flush performance is bad, could consider fetching in larger batches.
  // TODO: look at above - puts a minimum on real block size
  uint8_t buf_[32768];
  size_t batch_size_;
};

Status LayerWriter::Open() {
  CHECK(cfile_writers_.empty());

  // Create the directory for the new layer
  RETURN_NOT_OK(env_->CreateDir(dir_));


  for (int i = 0; i < schema_.num_columns(); i++) {
    const ColumnSchema &col = schema_.column(i);

    // TODO: allow options to be configured, perhaps on a per-column
    // basis as part of the schema. For now use defaults.
    //
    // Also would be able to set encoding here, or do something smart
    // to figure out the encoding on the fly.
    cfile::WriterOptions opts;

    // Index the key column by its value.
    if (i < schema_.num_key_columns()) {
      opts.write_validx = true;
    }
    // Index all columns by ordinal position, so we can match up
    // the corresponding rows.
    opts.write_posidx = true;

    string path = Layer::GetColumnPath(dir_, i);

    // Open file for write.
    shared_ptr<WritableFile> out;
    Status s = env_util::OpenFileForWrite(env_, path, &out);
    if (!s.ok()) {
      LOG(WARNING) << "Unable to open output file for column " <<
        col.ToString() << " at path " << path << ": " << 
        s.ToString();
      return s;
    }

    // Create the CFile writer itself.
    std::auto_ptr<cfile::Writer> writer(new cfile::Writer(
                                          opts,
                                          col.type_info().type(),
                                          cfile::GetDefaultEncoding(col.type_info().type()),
                                          out));

    s = writer->Start();
    if (!s.ok()) {
      LOG(WARNING) << "Unable to Start() writer for column " <<
        col.ToString() << " at path " << path << ": " << 
        s.ToString();
      return s;
    }

    LOG(INFO) << "Opened CFile writer for column " <<
      col.ToString() << " at path " << path;
    cfile_writers_.push_back(writer.release());
  }

  return Status::OK();
}

Status LayerWriter::InitBloomFileWriter(scoped_ptr<BloomFileWriter> *bfw) const {
  string path(Layer::GetBloomPath(dir_));
  shared_ptr<WritableFile> file;
  RETURN_NOT_OK( env_util::OpenFileForWrite(env_, path, &file) );
  bfw->reset(new BloomFileWriter(file, bloom_sizing_));
  return bfw->get()->Start();
}


Status LayerWriter::FlushProjection(const Schema &projection,
                                    RowIteratorInterface *src_iter,
                                    bool need_arena,
                                    bool write_bloom) {
  const Schema &iter_schema = src_iter->schema();

  CHECK(!finished_);
  vector<size_t> orig_projection;
  vector<size_t> iter_projection;
  projection.GetProjectionFrom(schema_, &orig_projection);
  projection.GetProjectionFrom(iter_schema, &iter_projection);

  faststring encoded_key_buf; // for blooms
  scoped_ptr<BloomFileWriter> bfw;
  if (write_bloom) {
    InitBloomFileWriter(&bfw);
  }


  ScopedBatchReader batcher(src_iter, need_arena);

  size_t written = 0;
  while (batcher.HasNext()) {
    // Read a batch from the iterator.
    size_t nrows;
    RETURN_NOT_OK(batcher.NextBatch(&nrows));
    CHECK_GT(nrows, 0);

    // Write the batch to the each of the columns
    for (int proj_col = 0; proj_col < projection.num_columns(); proj_col++) {
      size_t orig_col = orig_projection[proj_col];
      size_t iter_col = iter_projection[proj_col];

      size_t stride = iter_schema.byte_size();
      const void *p = batcher.col_ptr(iter_col);
      RETURN_NOT_OK( cfile_writers_[orig_col].AppendEntries(p, nrows, stride) );
    }

    // Write the batch to the bloom, if applicable
    if (write_bloom) {
      const uint8_t *row = batcher.row_ptr(0);
      for (size_t i = 0; i < nrows; i++) {
        // TODO: performance might be better if we actually batch this -
        // encode a bunch of key slices, then pass them all in one go.

        // Encode the row into sortable form
        encoded_key_buf.clear();
        Slice row_slice(row, iter_schema.byte_size());
        iter_schema.EncodeComparableKey(row_slice, &encoded_key_buf);

        // Insert the encoded row into the bloom.
        Slice encoded_key_slice(encoded_key_buf);
        RETURN_NOT_OK( bfw->AppendKeys(&encoded_key_slice, 1) );

        // Advance.
        row += iter_schema.byte_size();
      }
    }

    written += nrows;
  }

  // Finish columns.
  for (int proj_col = 0; proj_col < projection.num_columns(); proj_col++) {
    size_t orig_col = orig_projection[proj_col];
    CHECK_EQ(column_flushed_counts_[orig_col], 0);
    column_flushed_counts_[orig_col] = written;

    RETURN_NOT_OK(cfile_writers_[orig_col].Finish());
  }

  // Finish bloom.
  if (write_bloom) {
    RETURN_NOT_OK(bfw->Finish());
  }


  return Status::OK();
}

Status LayerWriter::Finish() {
  CHECK(!finished_);
  for (int i = 0; i < schema_.num_columns(); i++) {
    CHECK_EQ(column_flushed_counts_[i], column_flushed_counts_[0])
      << "Uneven flush. Column " << schema_.column(i).ToString() << " didn't match count "
      << "of column " << schema_.column(0).ToString();
    if (!cfile_writers_[i].finished()) {
      RETURN_NOT_OK(cfile_writers_[i].Finish());
    }
  }

  finished_ = true;

  return Status::OK();
}


////////////////////////////////////////////////////////////
// Reader
////////////////////////////////////////////////////////////

Status Layer::Open(Env *env,
                   const Schema &schema,
                   const string &layer_dir,
                   shared_ptr<Layer> *layer) {
  shared_ptr<Layer> l(new Layer(env, schema, layer_dir));

  RETURN_NOT_OK(l->Open());

  layer->swap(l);
  return Status::OK();
}


Layer::Layer(Env *env,
             const Schema &schema,
             const string &layer_dir) :
    env_(env),
    schema_(schema),
    dir_(layer_dir),
    open_(false),
    delta_tracker_(new DeltaTracker(env, schema, layer_dir))
{}


Status Layer::Open() {
  std::auto_ptr<CFileBaseData> new_base(
    new CFileBaseData(env_, dir_, schema_));
  RETURN_NOT_OK(new_base->OpenAllColumns());

  base_data_.reset(new_base.release());
  RETURN_NOT_OK(delta_tracker_->Open());

  open_ = true;

  return Status::OK();
}

Status Layer::FlushDeltas() {
  return delta_tracker_->Flush();
}

RowIteratorInterface *Layer::NewRowIterator(const Schema &projection) const {
  CHECK(open_);
  //boost::shared_lock<boost::shared_mutex> lock(component_lock_);
  // TODO: need to add back some appropriate locking?

  shared_ptr<RowIteratorInterface> base_iter(base_data_->NewRowIterator(projection));
  return delta_tracker_->WrapIterator(base_iter);
}

Status Layer::UpdateRow(const void *key,
                        const RowDelta &update) {
  CHECK(open_);

  uint32_t row_idx;
  RETURN_NOT_OK(base_data_->FindRow(key, &row_idx));
  delta_tracker_->Update(row_idx, update);

  return Status::OK();
}

Status Layer::CheckRowPresent(const LayerKeyProbe &probe,
                              bool *present) const {
  CHECK(open_);

  return base_data_->CheckRowPresent(probe, present);
}

Status Layer::CountRows(size_t *count) const {
  CHECK(open_);

  return base_data_->CountRows(count);
}

uint64_t Layer::EstimateOnDiskSize() const {
  CHECK(open_);
  // TODO: should probably add the delta trackers as well.
  return base_data_->EstimateOnDiskSize();
}


Status Layer::Delete() {
  // TODO: actually rm -rf, not just rename!
  return env_->RenameFile(dir_, dir_ + ".deleted");
}


////////////////////////////////////////

Status FlushInProgressLayer::Open(Env *env,
                                  const Schema &schema,
                                  const string &layer_dir,
                                  const shared_ptr<MemStore> &ms,
                                  shared_ptr<FlushInProgressLayer> *layer) {
  shared_ptr<FlushInProgressLayer> l(new FlushInProgressLayer(env, schema, layer_dir, ms));

  RETURN_NOT_OK(l->Open());

  layer->swap(l);
  return Status::OK();
}

FlushInProgressLayer::FlushInProgressLayer(Env *env,
                                           const Schema &schema,
                                           const string &dir,
                                           const shared_ptr<MemStore> &ms) :
  env_(env),
  dir_(dir),
  schema_(schema),
  base_data_(new CFileBaseData(env_, dir_, schema_)),
  delta_tracker_(new DeltaTracker(env, schema, dir)),
  ms_(ms),
  open_(false)
{
  always_locked_.lock();
}

FlushInProgressLayer::~FlushInProgressLayer() {
  always_locked_.unlock();
}

Status FlushInProgressLayer::Open() {
  RETURN_NOT_OK( base_data_->OpenKeyColumns() );
  open_ = true;
  return Status::OK();
}

RowIteratorInterface *FlushInProgressLayer::NewRowIterator(const Schema &projection) const {
  CHECK(open_);
  // Use memstore as base data, updated by delta tracker
  shared_ptr<RowIteratorInterface> base_iter(ms_->NewRowIterator(projection));
  return delta_tracker_->WrapIterator(base_iter);
}

Status FlushInProgressLayer::UpdateRow(const void *key,
                                       const RowDelta &update) {
  CHECK(open_);
  uint32_t row_idx;
  RETURN_NOT_OK(base_data_->FindRow(key, &row_idx));
  delta_tracker_->Update(row_idx, update);

  return Status::OK();
}

Status FlushInProgressLayer::CheckRowPresent(const LayerKeyProbe &probe,
                              bool *present) const {
  CHECK(open_);
  return ms_->CheckRowPresent(probe, present);
}

Status FlushInProgressLayer::CountRows(size_t *count) const {
  CHECK(open_);
  return base_data_->CountRows(count);
}

uint64_t FlushInProgressLayer::EstimateOnDiskSize() const {
  // The actual value of this doesn't matter, since it won't be selected
  // for compaction.
  return 0;
}


Status FlushInProgressLayer::Delete() {
  LOG(FATAL) << "Unsupported op";
  return Status::NotSupported("");
}

////////////////////////////////////////////////////////////
// CompactionInProgressLayer
////////////////////////////////////////////////////////////

Status CompactionInProgressLayer::Open(
  Env *env,
  const Schema &schema,
  const string &layer_dir,
  const LayerVector &input_layers,
  shared_ptr<CompactionInProgressLayer> *layer)
{
  shared_ptr<CompactionInProgressLayer> l(
    new CompactionInProgressLayer(env, schema, layer_dir, input_layers));

  RETURN_NOT_OK(l->Open());

  layer->swap(l);
  return Status::OK();
}

CompactionInProgressLayer::CompactionInProgressLayer(Env *env,
                                                     const Schema &schema,
                                                     const string &dir,
                                                     const LayerVector &input_layers) :
  env_(env),
  dir_(dir),
  schema_(schema),
  base_data_(new CFileBaseData(env_, dir_, schema_)),
  delta_tracker_(new DeltaTracker(env, schema, dir)),
  input_layers_(input_layers),
  open_(false)
{
  always_locked_.lock();
}

CompactionInProgressLayer::~CompactionInProgressLayer() {
  always_locked_.unlock();
}

Status CompactionInProgressLayer::Open() {
  RETURN_NOT_OK( base_data_->OpenKeyColumns() );
  open_ = true;
  return Status::OK();
}

RowIteratorInterface *CompactionInProgressLayer::NewRowIterator(const Schema &projection) const {
  CHECK(open_);

  // Use a union of the input layers as the row iterator.
  // No need to merge with the delta tracker, since we're duplicating all the writes
  // back into the input layers during the compaction.
  vector<shared_ptr<RowIteratorInterface> > iters;
  BOOST_FOREACH(const shared_ptr<LayerInterface> &layer, input_layers_) {
    shared_ptr<RowIteratorInterface> iter(layer->NewRowIterator(projection));
    iters.push_back(iter);
  }

  return new UnionIterator(iters);
}

Status CompactionInProgressLayer::UpdateRow(const void *key,
                                            const RowDelta &update) {
  CHECK(open_);

  uint32_t row_idx_in_output;
  RETURN_NOT_OK( base_data_->FindRow(key, &row_idx_in_output) );


  // Duplicate the update to both the relevant input layer and the output layer.
  // First propagate to the relevant input layer.
  bool updated = false;
  BOOST_FOREACH(shared_ptr<LayerInterface> &layer, input_layers_) {
    Status s = layer->UpdateRow(key, update);
    if (s.ok()) {
      updated = true;
      break;
    } else if (!s.IsNotFound()) {
      LOG(ERROR) << "Unable to update key "
                 << schema_.CreateKeyProjection().DebugRow(key)
                 << " (failed on layer " << layer->ToString() << "): "
                 << s.ToString();
      return s;
    }
  }

  if (!updated) {
    LOG(DFATAL) << "Found row in compaction output but not in any input layer: "
                << schema_.CreateKeyProjection().DebugRow(key);
    return Status::NotFound("not found in any input layer of compaction");
  }

  // Then put in the delta tracker corresponding to the post-compaction
  // row index.
  delta_tracker_->Update(row_idx_in_output, update);
  return Status::OK();
}

Status CompactionInProgressLayer::CheckRowPresent(const LayerKeyProbe &probe,
                              bool *present) const {
  return base_data_->CheckRowPresent(probe, present);
}

Status CompactionInProgressLayer::CountRows(size_t *count) const {
  CHECK(open_);
  return base_data_->CountRows(count);
}

uint64_t CompactionInProgressLayer::EstimateOnDiskSize() const {
  // The actual value of this doesn't matter, since it won't be selected
  // for compaction.
  return 0;
}


Status CompactionInProgressLayer::Delete() {
  LOG(FATAL) << "Unsupported op";
  return Status::NotSupported("");
}


} // namespace tablet
} // namespace kudu
