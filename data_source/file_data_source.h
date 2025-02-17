#ifndef FILEDATASOURCE_H
#define FILEDATASOURCE_H

#include "dpu_element/dpu_element.h"
#include "dpu_config/dpu_config.h"


class FileDataSource: public DPUElement
{
public:
  FileDataSource();
  ~FileDataSource();
  void Init(DPUConfig* dpu_config);
  void Run();
  void Stop();
  void ConfigureByPreviousElement(DPUConfig* dpu_config);
  void Process();
  void ElementMainLoop();
private:
  int64_t GetFileSize(const char* file_name);
  bool file_data_source_loop_;
  FILE* data_source_file_;
  char data_source_file_name_[MAX_FILENAME_LENGTH];
  int64_t data_source_file_size_;
  char* data_source_file_buffer_;
  int number_of_samples_in_one_window_;
  int window_size_in_bytes_;
  float read_time_interval_between_windos_in_ms_;
};

#endif // FILEDATASOURCE_H
