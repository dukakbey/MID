#include "data_source/file_data_source.h"
#include <csignal>
#include "dpu_config/dpu_config.h"

#if __MINGW32__
#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define __MSVCRT_VERSION__ 0x0601
#endif
#include <sys/stat.h>

FileDataSource::FileDataSource()
    : DPUElement()
{
}

FileDataSource::~FileDataSource()
{
  fclose(data_source_file_);
}

void FileDataSource::Init(DPUConfig* dpu_config)
{
  // lock the initialization mutex
  boost::unique_lock<boost::mutex> element_initialized_guard(element_initialized_mutex_);

  // lock the config access mutex
  boost::unique_lock<boost::mutex> config_access_guard(dpu_config->config_access_mutex_);
  // set internal DPU_config reference
  SetDPUConfig(dpu_config);
  file_data_source_loop_ = GetDPUConfig()->file_data_source_parameters_.file_data_source_loop;
  number_of_samples_in_one_window_ = GetDPUConfig()->preprocessor_parameters_.number_of_samples_in_one_window;
  read_time_interval_between_windos_in_ms_ = GetDPUConfig()->file_data_source_parameters_.read_time_interval_between_windos_in_ms;
  window_size_in_bytes_ = number_of_samples_in_one_window_ * GetDPUConfig()->file_data_source_parameters_.file_data_source_number_of_channels * 2;
  strncpy(data_source_file_name_, GetDPUConfig()->file_data_source_parameters_.file_data_source_name, MAX_FILENAME_LENGTH);
  config_access_guard.unlock();

  // get file size;
  data_source_file_size_ = GetFileSize(data_source_file_name_);

  // open data file
  data_source_file_ = fopen(data_source_file_name_, "rb");
  
  if (data_source_file_ == NULL)
  {
    std::cout << "Cannot open data source file: \"" << data_source_file_name_ << "\"\n";
    return;
  }
  else
  {
    std::cout << "Data source file \"" << data_source_file_name_ << "\" opened\n";
  }

  // signal the processing thread that the configuration is complete
  element_initialized_ = true;
}

void FileDataSource::Run()
{
  dpu_element_thread_ = boost::thread(&FileDataSource::Process, this);
}

void FileDataSource::Stop()
{
  dpu_element_thread_.interrupt();
  dpu_element_thread_.join();
  io_service_.stop();
}

void FileDataSource::Process()
{
  GetDPUConfig()->interrogator_data_parameters_.number_of_channels_in_one_sample = GetDPUConfig()->file_data_source_parameters_.file_data_source_number_of_channels;
  GetNextElement()->ConfigureByPreviousElement(GetDPUConfig());

  wake_up_timer_.expires_from_now(boost::posix_time::milliseconds(0));
  ElementMainLoop();
  try
  {
    io_service_.run();
  }
  catch (std::exception &e)
  {
    std::cout << "Exception: file_data_source: " << e.what() << std::endl;
  }
}

void FileDataSource::ElementMainLoop()
{
  boost::this_thread::interruption_point();
  static int64_t data_bytes_read = 0;
  static int32_t number_of_buffer_chunks = 1;
  while (GetNextElement()->free_buffer_queue_->read_available() >= number_of_buffer_chunks * sizeof(BufferChunk))
  {

    boost::this_thread::interruption_point();
    GetNextElement()->free_buffer_queue_->pop((char*) &free_buffer_chunk_, sizeof(BufferChunk));
    number_of_buffer_chunks = free_buffer_chunk_.number_of_chunks;
    data_source_file_buffer_ = (char*) free_buffer_chunk_.buffer_pointer;
    
    //window_size_in_bytes_ = 400*kanal sayýsý*2byte
    if (data_bytes_read + window_size_in_bytes_ <= data_source_file_size_)
    {
      fread(data_source_file_buffer_, 1, window_size_in_bytes_, data_source_file_);
      // pass data to next element and wait for process completion
      GetNextElement()->full_buffer_queue_->push((char*) &free_buffer_chunk_, sizeof(BufferChunk));

      data_bytes_read += window_size_in_bytes_;
      boost::this_thread::sleep(boost::posix_time::milliseconds(read_time_interval_between_windos_in_ms_));

      //bu komut gelen raw data sürekli baþtan oynatýlsýn için gerekli.
      if (file_data_source_loop_ && data_bytes_read + window_size_in_bytes_ > data_source_file_size_)
      {
        data_bytes_read = 0;
        rewind(data_source_file_);
      }
    }
    else
    {
      std::raise(SIGTERM);
    }
  }
  wake_up_timer_.expires_from_now(boost::posix_time::milliseconds(WAKE_UP_INTERVAL_IN_MS));
  wake_up_timer_.async_wait(boost::bind(&FileDataSource::ElementMainLoop, this));
}

void FileDataSource::ConfigureByPreviousElement(DPUConfig* dpu_config)
{

}

int64_t FileDataSource::GetFileSize(const char* file_name)
{
  struct __stat64 st;
  if (_stat64(file_name, &st) == 0)
    return st.st_size;
  return -1;
}


