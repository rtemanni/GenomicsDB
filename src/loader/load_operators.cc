/**
 * The MIT License (MIT)
 * Copyright (c) 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of 
 * this software and associated documentation files (the "Software"), to deal in 
 * the Software without restriction, including without limitation the rights to 
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of 
 * the Software, and to permit persons to whom the Software is furnished to do so, 
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all 
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS 
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR 
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER 
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN 
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#include "load_operators.h"
#include "json_config.h"

#define VERIFY_OR_THROW(X) if(!(X)) throw LoadOperatorException(#X);
#define ONE_GB (1024ull*1024ull*1024ull)

#ifdef DO_MEMORY_PROFILING
#include "memory_measure.h"
#endif

//LoaderOperatorBase functions
void LoaderOperatorBase::handle_intervals_spanning_partition_begin(const int64_t row, const int64_t begin, const int64_t end,
    const size_t cell_size, const void* cell_ptr)
{
  if(begin > m_column_partition.first)
  {
    if(!m_crossed_column_partition_begin) //first cross
    {
      m_crossed_column_partition_begin = true;
      //Determine all rows for which there is a valid interval intersecting with column begin
      //These intervals must be operated on
      std::vector<uint8_t*> copies_vector;
      for(auto i=0ull;i<m_last_end_position_for_row.size();++i)
      {
        if(m_last_end_position_for_row[i] >= 0)
          copies_vector.push_back(m_cell_copies[i]);
        else
          if(m_cell_copies[i])
            free(m_cell_copies[i]);
        m_cell_copies[i] = 0;
        m_last_end_position_for_row[i] = -1ll;
      }
      m_cell_copies.clear();
      //Sort the copies vector in column major order
      CellPointersColumnMajorCompare cmp;
      std::sort(copies_vector.begin(), copies_vector.end(), cmp);
      //Invoke the operator function for each of the cells
      for(auto*& cell_copy_ptr : copies_vector)
      {
        operate(reinterpret_cast<const void*>(cell_copy_ptr));
        free(cell_copy_ptr);
        cell_copy_ptr = 0;
      }
    }
    return;
  }
  //begin <= m_column_partition.first from now
  if(end >= m_column_partition.first)   //intersects current partition
  {
    //Copy the cell - since this is the latest interval that intersects the partition
    m_last_end_position_for_row[row] = end;
    m_cell_copies[row] = static_cast<uint8_t*>(realloc(m_cell_copies[row], cell_size));
    VERIFY_OR_THROW(m_cell_copies[row] && "Memory allocation failed while creating copy of cell");
    memcpy(m_cell_copies[row], cell_ptr, cell_size);
  }
  else //most recent interval ends before the partition - invalidate entry for this row
    m_last_end_position_for_row[row] = -1ll;
}

void LoaderOperatorBase::finish(const int64_t column_interval_end)
{
  if(!m_crossed_column_partition_begin) //did not ever cross the column partition beginning, force cross
    handle_intervals_spanning_partition_begin(0, INT64_MAX-1, INT64_MAX-1, 0, 0);
}

//LoaderArrayWriter - writes to TileDB arrays
LoaderArrayWriter::LoaderArrayWriter(const VidMapper* id_mapper, const std::string& config_filename, int rank)
  : LoaderOperatorBase(id_mapper->get_num_callsets()), m_array_descriptor(-1), m_schema(0), m_storage_manager(0)
{
  //Parse json configuration
  rapidjson::Document json_doc;
  std::ifstream ifs(config_filename.c_str());
  std::string str((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
  json_doc.Parse(str.c_str());
  if(json_doc.HasParseError())
    throw FileBasedVidMapperException(std::string("Syntax error in JSON file ")+config_filename);
  //recreate array flag
  bool recreate_array = (json_doc.HasMember("delete_and_create_tiledb_array")
      && json_doc["delete_and_create_tiledb_array"].GetBool()) ? true : false;
  JSONLoaderConfig json_config;
  json_config.read_from_file(config_filename);
  auto workspace = json_config.get_workspace(rank);
  auto array_name = json_config.get_array_name(rank);
  //Schema
  bool row_based_partitioning = json_config.is_partitioned_by_row();
  RowRange row_partition(0, json_config.get_max_num_rows_in_array()-1);
  if(row_based_partitioning)
  {
    row_partition = json_config.get_row_partition(rank);
    row_partition.second = std::min(row_partition.second, static_cast<int64_t>(json_config.get_max_num_rows_in_array()-1));
  }
  else
    m_column_partition = json_config.get_column_partition(rank);
  //default true
  bool compress_tiledb_array = (!json_doc.HasMember("compress_tiledb_array") || json_doc["compress_tiledb_array"].GetBool());
  id_mapper->build_tiledb_array_schema(m_schema, array_name, row_based_partitioning, row_partition, compress_tiledb_array);
  //Disable synced writes
  if(json_doc.HasMember("disable_synced_writes") && json_doc["disable_synced_writes"].GetBool())
    g_TileDB_enable_SYNC_write = 0;
  //Storage manager
  size_t segment_size = json_doc.HasMember("segment_size") ? json_doc["segment_size"].GetInt64() : 10u*1024u*1024u;
  m_storage_manager = new VariantStorageManager(workspace, segment_size);
  auto mode = recreate_array ? "w" : "a";
  //Check if array already exists
  m_array_descriptor = m_storage_manager->open_array(array_name, mode);
  //Array does not exist - define it first
  if(m_array_descriptor < 0)
  {
    size_t num_cells_per_tile = 1000u;
    if(json_doc.HasMember("num_cells_per_tile") && json_doc["num_cells_per_tile"].IsInt64())
      num_cells_per_tile = json_doc["num_cells_per_tile"].GetInt64();
    VERIFY_OR_THROW(m_storage_manager->define_array(m_schema, num_cells_per_tile) == TILEDB_OK
        && "Could not define TileDB array");
    //Open array in write mode
    m_array_descriptor = m_storage_manager->open_array(array_name, "w");
  }
  VERIFY_OR_THROW(m_array_descriptor != -1 && "Could not open TileDB array for loading");
  m_storage_manager->update_row_bounds_in_array(m_array_descriptor, row_partition.first,
      std::min(row_partition.second, id_mapper->get_max_callset_row_idx()));
}

#ifdef DUPLICATE_CELL_AT_END
void LoaderArrayWriter::write_top_element_to_disk()
{
  //Copy not reference
  CellWrapper top_element = m_cell_wrapper_pq.top();
  m_cell_wrapper_pq.pop();
  auto idx_in_vector = top_element.m_idx_in_cell_copies_vector;
  m_storage_manager->write_cell_sorted(m_array_descriptor,
      reinterpret_cast<const void*>(m_cell_copies[idx_in_vector]));
  //If this is a begin cell and spans multiple columns, retain this copy for the END in the PQ
  if(top_element.m_end_column > top_element.m_begin_column)
  {
    //swap begin/end
    std::swap<int64_t>(top_element.m_begin_column, top_element.m_end_column);
    //Update co-ordinate and END in the cell buffer
    auto* copy_ptr = m_cell_copies[idx_in_vector];
    //column is second co-ordinate
    *(reinterpret_cast<int64_t*>(copy_ptr+sizeof(int64_t))) = top_element.m_begin_column;
    //END is after co-ordinates and cell_size
    *(reinterpret_cast<int64_t*>(copy_ptr+2*sizeof(int64_t)+sizeof(size_t))) = top_element.m_end_column;
    //Add to PQ again
    m_cell_wrapper_pq.push(top_element);
  }
  else  //no need to keep this cell anymore, free "heap"
    m_memory_manager.push(idx_in_vector);
}
#endif

void LoaderArrayWriter::operate(const void* cell_ptr)
{
  assert(m_storage_manager);
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(cell_ptr);
  //row
  auto row = *(reinterpret_cast<const int64_t*>(ptr));
  //column is second co-ordinate
  auto column_begin = *(reinterpret_cast<const int64_t*>(ptr+sizeof(int64_t)));
  //END is after co-ordinates and cell_size
  auto column_end = *(reinterpret_cast<const int64_t*>(ptr+2*sizeof(int64_t)+sizeof(size_t)));
  assert(column_end >= column_begin && static_cast<size_t>(row) < m_last_end_position_for_row.size());
  //cell size is after co-ordinates
  auto cell_size = *(reinterpret_cast<const size_t*>(ptr+2*sizeof(int64_t)));
  if(!m_crossed_column_partition_begin)
  {
    handle_intervals_spanning_partition_begin(row, column_begin, column_end, cell_size, cell_ptr);
    if(!m_crossed_column_partition_begin)       //still did not cross
      return;
  }
#ifdef DUPLICATE_CELL_AT_END
  //Reason: the whole setup works only if the intervals for a given row/sample are non-overlapping. This
  //property must be enforced by the loader
  //We maintain the last END value seen for every row - if the new cell has a begin
  // <= last END, then we truncate the END to column_begin-1 and write out the END copy cell
  //However, we must ensure that the copy cell with the truncated END respects column order
  //Hence, we only write to disks those cells (and END cell copies) which are less than (row+1, column_begin-1)
  //That way a truncated cell copy will be inserted at the correct position
  //Note that this increases memory consumption and run-time as every cell needs to be copied here
  CellWrapper curr_cell_wrapper({row+1, column_begin-1, -1, 0ull});
  ColumnMajorCellCompareGT cmp_op_GT;
  //Loop till (row+1, column_begin-1) > PQ top and write the top element to disk
  while(!m_cell_wrapper_pq.empty() && cmp_op_GT(curr_cell_wrapper, m_cell_wrapper_pq.top()))
    write_top_element_to_disk();
  //Check whether the last END value for this row overlaps current cell
  //If yes, must flush the entry from the PQ, update its co-ordinate to be column_begin-1 and insert into PQ again
  //Hopefully, entering this if statement is NOT the common case
  if(m_last_end_position_for_row[row] >= column_begin)
  {
    std::vector<CellWrapper> tmp_wrapper_vector;
    auto found_element = false;
    while(!m_cell_wrapper_pq.empty() && !found_element)
    {
      auto& top_ref = m_cell_wrapper_pq.top();
      if(top_ref.m_row == row)
        found_element = true;
      tmp_wrapper_vector.push_back(top_ref);
      m_cell_wrapper_pq.pop();
    }
    assert(found_element && tmp_wrapper_vector.size() > 0u);
    //Handle the last element separately
    for(auto i=0ull;i+1u<tmp_wrapper_vector.size();++i)
      m_cell_wrapper_pq.push(tmp_wrapper_vector[i]);
    //The cell corresponding to this row
    auto& last_element = tmp_wrapper_vector.back();
    auto idx_in_vector = last_element.m_idx_in_cell_copies_vector;
    auto copy_ptr = m_cell_copies[idx_in_vector];
    //Should always be an END copy cell - why? Because if this is a valid begin cell, then
    //m_begin_column < column_begin and the cell would have been written to disk by the loop over
    //the PQ above
    if(last_element.m_end_column < last_element.m_begin_column) //end copy, update co-ordinate
    {
      assert(last_element.m_begin_column == m_last_end_position_for_row[row]);
      last_element.m_begin_column = column_begin-1;
      //if the END copy still is at a column > its begin position, then write to disk
      //Due to the way the loop over the PQ operates above, this END copy cell is the next cell to go to disk
      //If the END copy cell is at column == its begin position, then after truncation, the copy has become a 
      //single position cell and there is no need to write the END copy
      if(last_element.m_begin_column != last_element.m_end_column)
      {
        //column is second co-ordinate
        *(reinterpret_cast<int64_t*>(copy_ptr+sizeof(int64_t))) = last_element.m_begin_column;
        m_storage_manager->write_cell_sorted(m_array_descriptor, reinterpret_cast<const void*>(copy_ptr));
      }
      m_memory_manager.push(idx_in_vector); //"free" memory
    }
    else      //m_begin_column>=m_end_column>=column_begin, incorrect input data
      throw LoadOperatorException(std::string("ERROR: two cells in incorrect order found\nPrevious cell: ")+
          std::to_string(last_element.m_row)+", "+std::to_string(last_element.m_begin_column)+", "+
          std::to_string(last_element.m_end_column)+
          "\nNew cell: "+std::to_string(row)+", "+std::to_string(column_begin));
  }
  size_t idx_in_vector = 0ull;
  if(m_memory_manager.empty())        //no free entries, need to allocate a new block
  {
    idx_in_vector = m_cell_copies.size();
    m_cell_copies.push_back(static_cast<uint8_t*>(malloc(cell_size)));
  }
  else        //free entry
  {
    idx_in_vector = m_memory_manager.top();
    m_memory_manager.pop();
    //realloc to new size
    m_cell_copies[idx_in_vector] = static_cast<uint8_t*>(realloc(m_cell_copies[idx_in_vector], cell_size));
  }
  VERIFY_OR_THROW(m_cell_copies[idx_in_vector] && "Memory allocation failed while creating copy of cell");
  memcpy(m_cell_copies[idx_in_vector], ptr, cell_size);
  //Update the cell wrapper structure
  curr_cell_wrapper.m_row = row;
  curr_cell_wrapper.m_begin_column = column_begin;
  curr_cell_wrapper.m_end_column = column_end;
  curr_cell_wrapper.m_idx_in_cell_copies_vector = idx_in_vector;
  //insert CellWrapper pointer into PQ
  m_cell_wrapper_pq.push(curr_cell_wrapper);
  //Update last END value seen
  m_last_end_position_for_row[row] = column_end;
#else //ifdef DUPLICATE_CELL_AT_END
  m_storage_manager->write_cell_sorted(m_array_descriptor, cell_ptr);
#endif //ifdef DUPLICATE_CELL_AT_END
}

void LoaderArrayWriter::finish(const int64_t column_interval_end)
{
  LoaderOperatorBase::finish(column_interval_end);
#ifdef DUPLICATE_CELL_AT_END
  //some cells may be left in the PQ, write them to disk
  while(!m_cell_wrapper_pq.empty())
    write_top_element_to_disk();
#endif
  if(m_storage_manager && m_array_descriptor >= 0)
    m_storage_manager->close_array(m_array_descriptor);
}

#ifdef HTSDIR
LoaderCombinedGVCFOperator::LoaderCombinedGVCFOperator(const VidMapper* id_mapper, const std::string& config_filename,
    bool handle_spanning_deletions, int partition_idx, const ColumnRange& partition_range)
  : LoaderOperatorBase(id_mapper->get_num_callsets()), m_schema(0), m_query_processor(0), m_operator(0)
{
  clear(); 
  //Loader configuration
  JSONLoaderConfig loader_json_config;
  loader_json_config.read_from_file(config_filename);
  if(!loader_json_config.is_partitioned_by_row())
    m_column_partition = loader_json_config.get_column_partition(partition_idx);
  //initialize arguments
  m_vid_mapper = id_mapper;
  //initialize query processor
  m_vid_mapper->build_tiledb_array_schema(m_schema, "", false, RowRange(0, id_mapper->get_num_callsets()-1), false);
  m_query_processor = new VariantQueryProcessor(*m_schema);
  //Initialize query config
  std::vector<std::string> query_attributes(m_schema->attribute_num());
  for(auto i=0ull;i<m_schema->attribute_num();++i)
    query_attributes[i] = m_schema->attribute_name(i);
  m_query_config.set_attributes_to_query(query_attributes);
  m_query_processor->do_query_bookkeeping(*m_schema, m_query_config);
  //Initialize VCF adapter
  if(loader_json_config.offload_vcf_output_processing())
  {
    m_offload_vcf_output_processing = true;
    //2 entries in circular buffer, max #entries to use in each line_buffer
    m_buffered_vcf_adapter = new BufferedVCFAdapter(2u, m_vid_mapper->get_num_callsets());
    m_vcf_adapter = dynamic_cast<VCFAdapter*>(m_buffered_vcf_adapter);
  }
  else
  {
    m_offload_vcf_output_processing = false;
    m_vcf_adapter = new VCFAdapter();
    m_buffered_vcf_adapter = 0;
  }
  JSONVCFAdapterConfig vcf_adapter_config;
  vcf_adapter_config.read_from_file(config_filename, *m_vcf_adapter, "", partition_idx);
  //Initialize operator
  if(vcf_adapter_config.get_determine_sites_with_max_alleles() > 0)
    m_operator = new MaxAllelesCountOperator(vcf_adapter_config.get_determine_sites_with_max_alleles());
  else
    m_operator = new BroadCombinedGVCFOperator(*m_vcf_adapter, *m_vid_mapper, m_query_config,
        vcf_adapter_config.get_max_diploid_alt_alleles_that_can_be_genotyped());
  //Initialize variant
  m_variant = std::move(Variant(&m_query_config));
  m_variant.resize_based_on_query();
  //Cell
  m_cell = new BufferVariantCell(*m_schema, m_query_config);
  //Partition bounds
  m_partition = partition_range;
  //PQ elements
  m_tmp_pq_vector.resize(m_query_config.get_num_rows_to_query());
  //Position elements
  m_current_start_position = -1ll;
  m_next_start_position = -1ll;
  //Deletion flags
  m_num_calls_with_deletions = 0;
  m_handle_spanning_deletions = handle_spanning_deletions;
  //Profiling
#ifdef DO_PROFILING
  m_stats_ptr = &m_stats;
#else
  m_stats_ptr = 0;
#endif
#ifdef DO_MEMORY_PROFILING
  m_next_memory_limit = ONE_GB;
#endif
}

void LoaderCombinedGVCFOperator::clear()
{
  m_query_config.clear();
  m_variant.clear();
  m_tmp_pq_vector.clear();
}

void LoaderCombinedGVCFOperator::operate(const void* cell_ptr)
{
  auto coords = reinterpret_cast<const int64_t*>(cell_ptr);
  auto row = coords[0];
  auto column_begin = coords[1];
  auto ptr = reinterpret_cast<const uint8_t*>(cell_ptr);
  //Cell size after the coords
  auto cell_size = *(reinterpret_cast<const size_t*>(ptr+2*sizeof(int64_t)));
  //END value is after cooords and cell_size
  ptr += 2*sizeof(int64_t)+sizeof(size_t);
  auto column_end = *(reinterpret_cast<const int64_t*>(ptr));
  if(!m_crossed_column_partition_begin)
  {
    handle_intervals_spanning_partition_begin(row, column_begin, column_end, cell_size, cell_ptr);
    if(!m_crossed_column_partition_begin)       //still did not cross
      return;
  }
  //Either un-initialized or VariantCall interval starts before/at partition begin value
  if(m_current_start_position < 0 || column_begin <= m_partition.first)
  {
    m_current_start_position = column_begin;
    m_variant.set_column_interval(column_begin, column_begin);
  }
  else  //column_begin > m_partition.first, check if m_current_start_position < m_partition.first
    if(m_current_start_position < m_partition.first)
    {
      m_current_start_position = m_partition.first;
      m_variant.set_column_interval(m_current_start_position, m_current_start_position);
    }
  m_cell->set_cell(cell_ptr);
  m_query_processor->scan_handle_cell(m_query_config, 0u,
      m_variant, *m_operator, *m_cell,
      m_end_pq, m_tmp_pq_vector,
      m_current_start_position, m_next_start_position,
      m_num_calls_with_deletions, m_handle_spanning_deletions,
      m_stats_ptr);
#ifdef DO_MEMORY_PROFILING
  statm_t mem_result;
  read_off_memory_status(mem_result);
  if(mem_result.resident >= m_next_memory_limit)
  {
    std::cerr << "Crossed "<<m_next_memory_limit<<" at position "<<column_begin<<"\n";
    m_next_memory_limit += ONE_GB;
  }
#endif
  return;
}

void LoaderCombinedGVCFOperator::finish(const int64_t column_interval_end)
{
  LoaderOperatorBase::finish(column_interval_end);
  assert(!m_offload_vcf_output_processing || m_buffered_vcf_adapter->get_num_entries_with_valid_data() == 0u);
  pre_operate_sequential();
  //Fix start and next_start positions if necessary
  if(m_current_start_position < m_partition.first)
  {
    m_current_start_position = m_partition.first;
    m_variant.set_column_interval(m_current_start_position, m_current_start_position);
  }
  m_next_start_position = (column_interval_end == INT64_MAX) ? INT64_MAX : column_interval_end+1;
  auto operator_overflow = true;
  while(operator_overflow)
  {
    m_query_processor->handle_gvcf_ranges(m_end_pq, m_query_config, m_variant, *m_operator,
        m_current_start_position, m_next_start_position, column_interval_end == INT64_MAX, m_num_calls_with_deletions);
    operator_overflow = m_operator->overflow(); //must be queried before post_operate_sequential and flush_output() are called
#ifdef DO_MEMORY_PROFILING
    statm_t mem_result;
    read_off_memory_status(mem_result);
    if(mem_result.resident > m_next_memory_limit)
    {
      std::cerr << "ENDING crossed "<<m_next_memory_limit<<"\n";
      m_next_memory_limit += ONE_GB;
    }
#endif
    post_operate_sequential();
    flush_output();
  }
}

#endif
