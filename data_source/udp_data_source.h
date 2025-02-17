#ifndef UDP_DATA_SOURCE_H_
#define UDP_DATA_SOURCE_H_

#define CHECK_REF_FILE
#define MEASURE_THROUGHPUT

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/array.hpp>

#include "dpu_element/dpu_element.h"
#include "dpu_config/dpu_config.h"
// using boost::asio::ip::udp;

#include "dpu_element/packed.h"
struct UDPPacketHeader
{
  uint16_t message_ID;
  uint32_t frame_count;
  uint16_t total_fragment_count;
  uint16_t frame_fragment_no;
  uint16_t info;
  uint16_t message_length;
} PACKED;


struct UDPPacketInfoTyp
{
	uint16_t Reserved : 15;
	uint16_t InputNo : 1;
}PACKED;


class UDPDataSource: public DPUElement
{
public:
  UDPDataSource();
  ~UDPDataSource();
  void Init(DPUConfig* dpu_config);
  void Run();
  void Stop();
  void Process();
  void ElementMainLoop();
  void ConfigureByPreviousElement(DPUConfig* dpu_config);

private:
  void StartReceive();
  void HandleReceive(const boost::system::error_code& error, std::size_t bytes_transferred);
  void InitializeState(UDPPacketHeader* header);
  void ParseUDPPacketHeader();
  void ComputeFrameOffsetHashTable(uint32_t window_start_frame_count);
  int64_t ComputeHashTableIndex(uint32_t frame_count);
  void WritePacketToWindowBuffer(char* packet_buffer, char* window_buffer, uint32_t write_offset, uint32_t packet_size);
  void ConcealLostSamples();
  void CompareToReferenceFile(int64_t file_pos);
  
  int64_t GetFileSize(const char* file_name);
  void ReverseInt16Arr(int16_t *pArrInt16);

private:
  int check_configure_by_previous_element_in_first_time_;
  boost::asio::ip::udp::socket* socket_;
  boost::asio::ip::udp::endpoint remote_endpoint_;

  // set data params
  int32_t number_of_samples_in_one_window_;
  int32_t number_of_channels_in_one_sample_;
  int32_t window_size_in_bytes_;
  int32_t max_window_size_in_bytes_;
  int32_t max_packet_size_in_bytes_;
  int32_t non_final_fragment_size_in_bytes_;
  char* temporary_window_buffer_;
  char* window_buffer_;
  char* packet_buffer_;
  char* last_intact_sample_buffer_;
  int16_t *pTempArr_;
  int32_t port_;
  bool reset_parameters_;
  uint32_t window_start_frame_count_;
  uint32_t window_end_frame_count_;
  uint16_t number_of_frame_fragments_;
  uint16_t number_of_channels_in_frame_read_;
  uint32_t* window_frame_offsets_table_;
  uint16_t* window_fragment_count_table_;
  int64_t window_counter_;
  int32_t packet_header_size_;
  int32_t sample_frequency_;
  bool big_endian_;
  UDPPacketHeader* UDP_packet_header_;

  // throughput measurement
  boost::posix_time::ptime start_time_, end_time_;
  int64_t throughput_measurement_bytes_read_;
  FILE* reference_data_file_;
  
  int64_t reference_data_file_size_;
  char* reference_data_file_buffer_;
  bool reference_file_comparison_completed_;
  int64_t reference_file_bytes_read_;

  int interrogator_usage_type_;
  int32_t FragmentSzArr[100]; //Value of 100 should be examined!!!
  int input_priority_;

};
#include "dpu_element/end_packed.h"

#endif //UDP_DATA_SOURCE_H_
