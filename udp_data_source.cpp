// udp_data_source.cpp : Defines the entry point for the console application.
//
#include "data_source/udp_data_source.h"
#include <limits.h>
#include <iostream>
#include <fstream>
#include "dpu_config/dpu_config.h"
#include "logger/logger_factory.h"

#if __MINGW32__
#define __USE_LARGEFILE64
#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define __MSVCRT_VERSION__ 0x0601
#endif
#include <sys/stat.h>

using namespace std;
using namespace boost::interprocess;
using boost::asio::ip::udp;

UDPDataSource::UDPDataSource()
  : DPUElement()
{

	memset(this->FragmentSzArr, 0, sizeof(this->FragmentSzArr));
}

UDPDataSource::~UDPDataSource()
{
  // close the udp socket
  socket_->close();
  delete socket_;
  if (pTempArr_!=NULL) {
	  delete pTempArr_;
  }
  free(temporary_window_buffer_);
  free(packet_buffer_);
  free(last_intact_sample_buffer_);
  free(window_frame_offsets_table_);
  free(window_fragment_count_table_);
}

void UDPDataSource::Run()
{
  dpu_element_thread_ = boost::thread(&UDPDataSource::Process, this);
}

void UDPDataSource::Stop()
{
  dpu_element_thread_.interrupt();
  dpu_element_thread_.join();
  io_service_.stop();
}

void UDPDataSource::Process()
{
  std::cout << "UDP Data Source: Listening UDP Port: " << port_ << "\n";
  StartReceive();

  wake_up_timer_.expires_from_now(boost::posix_time::milliseconds(0));
  ElementMainLoop();
  try
  {
    io_service_.run();
  }
  catch (std::exception &e)
  {
	LOG_DEBUG << "Exception udp_data_source thread : " + std::string(e.what());
    std::cout << "Exception: udp_data_source: " << e.what() << std::endl;
  }
}

void UDPDataSource::ElementMainLoop()
{
  boost::this_thread::interruption_point();
  wake_up_timer_.expires_from_now(boost::posix_time::milliseconds(WAKE_UP_INTERVAL_IN_MS));
  wake_up_timer_.async_wait(boost::bind(&UDPDataSource::ElementMainLoop, this));
}

void UDPDataSource::ConfigureByPreviousElement(DPUConfig* dpu_config)
{

}

void UDPDataSource::Init(DPUConfig* dpu_config)
{
  boost::unique_lock<boost::mutex> element_initialized_guard(element_initialized_mutex_);

  // lock the config access mutex
  boost::unique_lock<boost::mutex> config_access_guard(dpu_config->config_access_mutex_);
  // set internal DPU_config reference
  SetDPUConfig(dpu_config);
  // set parameters in the configuration file
  port_ = GetDPUConfig()->interrogator_communication_parameters_.data_dest_port;
  big_endian_ = GetDPUConfig()->interrogator_communication_parameters_.rawdata_is_big_endian;
  check_configure_by_previous_element_in_first_time_ = 1;

  number_of_samples_in_one_window_ = GetDPUConfig()->preprocessor_parameters_.number_of_samples_in_one_window;
  interrogator_usage_type_ = GetDPUConfig()->interrogator_fiber_parameters_.interrogator_usage_type;
  input_priority_ = GetDPUConfig()->interrogator_fiber_parameters_.midas3_fiber_parameter.input_priority;
  config_access_guard.unlock();

  pTempArr_ = NULL;

  reset_parameters_ = 1;
  window_start_frame_count_ = UINT_MAX;
  window_end_frame_count_ = UINT_MAX;
  number_of_frame_fragments_ = 0;
  number_of_channels_in_frame_read_ = 0;
  sample_frequency_ = 2500;

  max_window_size_in_bytes_ = number_of_samples_in_one_window_ * MAX_NUMBER_OF_CHANNELS_IN_ONE_SAMPLE * 2;
  max_packet_size_in_bytes_ = 65507;
  window_counter_ = 0;

  // allocate buffer for samples
  temporary_window_buffer_ = (char*)malloc(max_window_size_in_bytes_);
  packet_buffer_ = (char*)malloc(max_packet_size_in_bytes_);
  last_intact_sample_buffer_ = (char*)malloc(MAX_NUMBER_OF_CHANNELS_IN_ONE_SAMPLE * 2);

  // allocate buffer for offset hash tables
  window_frame_offsets_table_ = (unsigned int*)malloc(number_of_samples_in_one_window_ * sizeof(unsigned int));

  // allocate buffer for fragment count table
  window_fragment_count_table_ = (unsigned short*)malloc(number_of_samples_in_one_window_ * sizeof(unsigned short));
  memset(window_fragment_count_table_, 0, number_of_samples_in_one_window_ * sizeof(unsigned short));

  packet_header_size_ = sizeof(UDPPacketHeader);
  UDP_packet_header_ = (UDPPacketHeader*)packet_buffer_;


  socket_ = new udp::socket(io_service_, udp::endpoint(udp::v4(), port_));
  boost::asio::socket_base::receive_buffer_size option(10 * sample_frequency_ * MAX_NUMBER_OF_CHANNELS_IN_ONE_SAMPLE * 2);
  socket_->set_option(option);
  // read receive buffer size
  /*
  socket_->get_option(option);
  std::cout << "Receive Buf Size: " << option.value() << "\n";
  */
  // signal the processing thread that the configuration is complete
  element_initialized_ = true;
  element_initialized_condition_.notify_one();
}

void UDPDataSource::StartReceive()
{
  try
  {
    socket_->async_receive_from(boost::asio::buffer(packet_buffer_, max_packet_size_in_bytes_),
      remote_endpoint_,
      boost::bind(&UDPDataSource::HandleReceive, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred)
    );
  }
  catch (std::exception& e)
  {
	LOG_DEBUG << "udp_data_source error : " + std::string(e.what());
    printf("\n\n Catched\n\n");
  }
}

void UDPDataSource::HandleReceive(const boost::system::error_code& error, std::size_t bytes_transferred)
{
  if (error && error != boost::asio::error::message_size)
  {
    throw boost::system::system_error(error);
  }
  else
  {
    
      // parse packet header
      ParseUDPPacketHeader();

#if 0
      // temporary code to fix 8 byte overhead by NANOTAM
      UDP_packet_header_->message_length = bytes_transferred;

      std::cout << "message_ID: " << UDP_packet_header_->message_ID;
      std::cout << " frame_count: " << UDP_packet_header_->frame_count;
      std::cout << " total_fragment_count: " << UDP_packet_header_->total_fragment_count;
      std::cout << " frame_fragment_no: " << UDP_packet_header_->frame_fragment_no;
      std::cout << " info: " << UDP_packet_header_->info;
      std::cout << " message_length: " << UDP_packet_header_->message_length;
      std::cout << std::endl;

#endif
      uint16_t filter = 1024; // get the bit 11
      uint16_t oamp_gain_state = (UDP_packet_header_->info & filter) >> 10;

  
      // reset parameters and counters
      if (reset_parameters_ == 1)
      {
        InitializeState(UDP_packet_header_);
      }
      else     // done with dynamic parameter initialization
      {
        // if the packet is in the future
        int64_t end_cnt_diff = ((int64_t)UDP_packet_header_->frame_count) - ((int64_t)window_end_frame_count_);
        if (end_cnt_diff > 0 || end_cnt_diff < INT_MIN)//400 sample'a ulaştığı zaman bu if'e giriyor.
        {
          // conceal lost samples
           ConcealLostSamples();

          // set the oamp_gain_change_status
          free_buffer_chunk_.oamp_gain_state = oamp_gain_state;
          // push filled buffer chunk to preprocessor if the data source popped a valid free buffer
          if (free_buffer_chunk_.buffer_pointer != NULL)
          {

			  switch (this->interrogator_usage_type_)
			  {
			  case REDUNDANT:
			  case SEPERATE:
			  {
				  int16_t *pArrU16 = (int16_t *)free_buffer_chunk_.buffer_pointer;

				  for (int i = 0; i < number_of_samples_in_one_window_; i++)
				  {
					  ReverseInt16Arr((int16_t *)&pArrU16[number_of_channels_in_one_sample_ * i]);
				  }
				  break;
			  }
			 
			  default:
				  break;
			  }
            GetNextElement()->full_buffer_queue_->push((char*)&free_buffer_chunk_, sizeof(BufferChunk));
          }

          // set the fragment count table to 0
          memset(window_fragment_count_table_, 0, number_of_samples_in_one_window_ * sizeof(unsigned short));
          window_counter_ += 1;

          // fill window frame offset hash table
          ComputeFrameOffsetHashTable(window_end_frame_count_ + 1);
          // reset the hash table if the received frame is not next window as well
          end_cnt_diff = ((int64_t)UDP_packet_header_->frame_count) - ((int64_t)window_end_frame_count_);
          if (end_cnt_diff > 0 || end_cnt_diff < INT_MIN)
          {
            std::cout << "Window: " << window_counter_ << " - " << "Bulk: " << end_cnt_diff + number_of_samples_in_one_window_ - 1 << " samples" << std::endl;
            ComputeFrameOffsetHashTable(UDP_packet_header_->frame_count);
          }
          // get the next free buffer
          if (GetNextElement()->free_buffer_queue_->pop((char*)&free_buffer_chunk_, sizeof(BufferChunk)) == sizeof(BufferChunk))
          {
            window_buffer_ = free_buffer_chunk_.buffer_pointer;
          }
          else
          {
            // there is no free buffer, write packets to a temporary memory location that will be ignored
            window_buffer_ = temporary_window_buffer_;
            free_buffer_chunk_.buffer_pointer = NULL;
			LOG_DEBUG << "****  there is no free buffer, write packets to a temporary memory location that will be ignored ****";
          }
        }
        int64_t start_cnt_diff = ((int64_t)UDP_packet_header_->frame_count) - ((int64_t)window_start_frame_count_);//400'e ulaşmadığı zaman buradaki yapı çalışıyor.
        // if the packet is not in the past
        if (start_cnt_diff >= 0 || start_cnt_diff < INT_MIN)
        {
          // calculate hash table index
          int64_t idx = ComputeHashTableIndex(UDP_packet_header_->frame_count);
          // calculate write index
          unsigned int write_offset = window_frame_offsets_table_[idx] + ((UDP_packet_header_->frame_fragment_no - 1) * non_final_fragment_size_in_bytes_);
		  switch (this->interrogator_usage_type_)
		  {

		  case REDUNDANT:
		  case SEPERATE:
		  {
			  write_offset = 0;
			  for (int32_t i = 0; i < (UDP_packet_header_->frame_fragment_no - 1); i++)
			  {
				  write_offset = write_offset + this->FragmentSzArr[i];
			  }
			  write_offset = write_offset + window_frame_offsets_table_[idx];
			  break;
		  }
		 
		  default: 
			  break;
		  }
		  // write packet to window buffer
          WritePacketToWindowBuffer(packet_buffer_, window_buffer_, write_offset, bytes_transferred);

          // update fragment counter
          window_fragment_count_table_[idx]++;

        }
        //else if (start_cnt_diff < -100000) // reset udp data source state when interrogation is restarted
        //{
        //  reset_parameters_ = 1;
        //}
		else if (start_cnt_diff < -10000) {
			if (UDP_packet_header_->frame_count==0) {
				window_start_frame_count_ = 0;
			}
			reset_parameters_ = 1;
		}
      }

    StartReceive();
  }
}

void UDPDataSource::InitializeState(UDPPacketHeader* UDP_packet_header)
{
  if (window_start_frame_count_ != UDP_packet_header->frame_count)
  {
    // initialize fragment counters
    window_start_frame_count_ = UDP_packet_header->frame_count;
    number_of_frame_fragments_ = 1;
    if (UDP_packet_header->frame_fragment_no == 1)
    {
      non_final_fragment_size_in_bytes_ = UDP_packet_header->message_length - packet_header_size_;
    }
    number_of_channels_in_frame_read_ = (UDP_packet_header->message_length - packet_header_size_) / 2;

  }
  else
  {
    number_of_frame_fragments_++;
    number_of_channels_in_frame_read_ += (UDP_packet_header->message_length - packet_header_size_) / 2;

    if (UDP_packet_header_->frame_fragment_no == 1)
    {
      non_final_fragment_size_in_bytes_ = UDP_packet_header->message_length - packet_header_size_;
    }
    // all fragments received
    if (number_of_frame_fragments_ == UDP_packet_header->total_fragment_count)
    {
      number_of_channels_in_one_sample_ = number_of_channels_in_frame_read_;
      window_size_in_bytes_ = number_of_samples_in_one_window_ * number_of_channels_in_one_sample_ * 2;
      std::cout << "UDP Data Source: Started to receive data for " << number_of_channels_in_one_sample_ << " channels, 16 bits/channel" << "\n";
	  // initialize frame starting from the next sample 
      ComputeFrameOffsetHashTable(UDP_packet_header->frame_count + 1);
      window_counter_ = 0;
      reset_parameters_ = 0;

      // update next dpu_elements configuration and initialize it
      // lock the config access mutex
      boost::unique_lock<boost::mutex> config_access_guard(GetDPUConfig()->config_access_mutex_);
      GetDPUConfig()->interrogator_data_parameters_.number_of_channels_in_one_sample = number_of_channels_in_one_sample_;
	  pTempArr_ = new int16_t[GetDPUConfig()->interrogator_data_parameters_.number_of_channels_in_one_sample / 2];
      config_access_guard.unlock();


	  if (check_configure_by_previous_element_in_first_time_) {
		  GetNextElement()->ConfigureByPreviousElement(GetDPUConfig());
		  check_configure_by_previous_element_in_first_time_ = 0;
	  }
      // get the next free buffer
	  if (GetNextElement()->free_buffer_queue_->pop((char*)&free_buffer_chunk_, sizeof(BufferChunk)) == sizeof(BufferChunk))
	  {
		  window_buffer_ = free_buffer_chunk_.buffer_pointer;
	  }

    }
  }
}

void UDPDataSource::ParseUDPPacketHeader()
{
  if (big_endian_)
  {
    UDP_packet_header_->message_ID = ntohs(UDP_packet_header_->message_ID);
    UDP_packet_header_->frame_count = ntohl(UDP_packet_header_->frame_count);
    UDP_packet_header_->total_fragment_count = ntohs(UDP_packet_header_->total_fragment_count);
    UDP_packet_header_->frame_fragment_no = ntohs(UDP_packet_header_->frame_fragment_no);
    UDP_packet_header_->info = ntohs(UDP_packet_header_->info);
    UDP_packet_header_->message_length = ntohs(UDP_packet_header_->message_length);

	switch (this->interrogator_usage_type_)
	{

	    case REDUNDANT:
		case SEPERATE:
		{
			UDPPacketInfoTyp *pInfo;
			pInfo = (UDPPacketInfoTyp *)&UDP_packet_header_->info;

			//cout <<"\n"<< UDP_packet_header_->frame_count << "--> ChannelNo :" << pInfo->InputNo << endl;

			//if (pInfo->InputNo == 1) //InputB
			//if (pInfo->InputNo == 0) //InputA
			

			if (pInfo->InputNo !=this->input_priority_)
			{
				UDP_packet_header_->frame_fragment_no += UDP_packet_header_->total_fragment_count;
			}

		    UDP_packet_header_->total_fragment_count *= 2;
			UDP_packet_header_->frame_count = (UDP_packet_header_->frame_count + 1) / 2;
		  
			this->FragmentSzArr[UDP_packet_header_->frame_fragment_no - 1] = UDP_packet_header_->message_length - sizeof(UDPPacketHeader);

			//cout << UDP_packet_header_->frame_count << "--> ChannelNo :" << pInfo->InputNo << "  frame_fragment_no: " << UDP_packet_header_->frame_fragment_no << endl;		 

			//{
			//string fileFullPathStr3 = "frame_count.dat";
			//WriteFileStream writeFileStream(fileFullPathStr3,
			//	std::ios::app | std::ios::binary,
			//	(char *)&UDP_packet_header_->frame_count,
			//	sizeof(int32_t));
			//}

			break;
		}
		default:
			break;
	}
  }
}

void UDPDataSource::ComputeFrameOffsetHashTable(uint32_t window_start_frame_count)
{
  uint32_t seq;
  int64_t idx;
  window_start_frame_count_ = window_start_frame_count;
  window_end_frame_count_ = window_start_frame_count_ + number_of_samples_in_one_window_ - 1;
  if (window_start_frame_count_ <= UINT_MAX - number_of_samples_in_one_window_)
  {
    for (int i = 0; i < number_of_samples_in_one_window_; i++)
    {
      seq = window_start_frame_count_ + i;
      idx = seq % number_of_samples_in_one_window_;
      window_frame_offsets_table_[idx] = i * number_of_channels_in_one_sample_ * 2;
    }
  }
  else
  {
    for (int i = 0; i < number_of_samples_in_one_window_; i++)
    {
      seq = window_start_frame_count_ + i;
      if (seq <= number_of_samples_in_one_window_)
      {
        idx = seq % number_of_samples_in_one_window_;
        window_frame_offsets_table_[idx] = i * number_of_channels_in_one_sample_ * 2;
      }
      else
      {
        idx = (seq - UINT_MAX - 1 + number_of_samples_in_one_window_) % number_of_samples_in_one_window_;
        window_frame_offsets_table_[idx] = i * number_of_channels_in_one_sample_ * 2;
      }
    }
  }

}

int64_t UDPDataSource::ComputeHashTableIndex(unsigned int frame_count)
{
  int64_t idx;
  if (window_start_frame_count_ <= UINT_MAX - number_of_samples_in_one_window_)
  {
    idx = frame_count % number_of_samples_in_one_window_;
  }
  else
  {
    if (frame_count <= number_of_samples_in_one_window_)
    {
      idx = frame_count % number_of_samples_in_one_window_;
    }
    else
    {
      idx = (frame_count - UINT_MAX - 1 + number_of_samples_in_one_window_) % number_of_samples_in_one_window_;
    }
  }
  return idx;
}

void UDPDataSource::WritePacketToWindowBuffer(char* packet_buffer, char* window_buffer, unsigned int write_offset, unsigned int packet_size)
{
   if (window_buffer == NULL) {
	   return;
  }
  if (big_endian_)
  {
    uint16_t* inDataUint16Buf = (uint16_t*)(packet_buffer + packet_header_size_);
    uint16_t* outDataUint16Buf = (uint16_t*)(window_buffer + write_offset);
    for (int i = 0; i < (packet_size - packet_header_size_) / 2; i++)
    {
      *(outDataUint16Buf + i) = ntohs(*(inDataUint16Buf + i));
    }
  }
  else
  {
    memcpy(window_buffer + write_offset, packet_buffer + packet_header_size_, packet_size - packet_header_size_);
  }
}

void UDPDataSource::ConcealLostSamples()
{
  int32_t number_of_lost_samples;
  uint32_t seq;
  int64_t idx;
  int64_t prev_idx;

  number_of_lost_samples = 0;
  // check whether the first sample of the window is lost
  idx = ComputeHashTableIndex(window_start_frame_count_);
  // at least one fragment of the sample is lost
  if (window_fragment_count_table_[idx] != number_of_frame_fragments_)
  {
    // copy last_intact_sample_buffer_ to lost sample
    memcpy((char*)(window_buffer_ + window_frame_offsets_table_[idx]), (char*)last_intact_sample_buffer_, number_of_channels_in_one_sample_ * 2);
    number_of_lost_samples++;
  }

  // check whether remaining samples in the window are lost
  prev_idx = idx;
  for (int i = 1; i < number_of_samples_in_one_window_; i++)
  {
    seq = window_start_frame_count_ + i;
    idx = ComputeHashTableIndex(seq);
    // at least one fragment of the sample is lost
    if (window_fragment_count_table_[idx] != number_of_frame_fragments_)
    {
      // copy previous sample to lost sample
      memcpy((char*)(window_buffer_ + window_frame_offsets_table_[idx]), (char*)(window_buffer_ + window_frame_offsets_table_[prev_idx]), number_of_channels_in_one_sample_ * 2);
      number_of_lost_samples++;
    }
    prev_idx = idx;
  }
  // copy the last sample of the window to the last_intact_sample_buffer_
  memcpy(last_intact_sample_buffer_, (char*)(window_buffer_ + window_frame_offsets_table_[idx]), number_of_channels_in_one_sample_ * 2);
  // print loss info
  if (number_of_lost_samples > 0)
  {
    std::cout << "Window: " << window_counter_ << " - " << "Lost: " << number_of_lost_samples << " samples" << std::endl;
  }
}


void UDPDataSource::CompareToReferenceFile(int64_t file_pos)
{
  int n = memcmp(reference_data_file_buffer_, window_buffer_, window_size_in_bytes_);
  if (n == 0)
  {
    std::cout << "Comp: EQ - ";
  }
  else
  {
    std::cout << "Comp: DIFF - ";
  }
  std::cout << "Time: " << window_counter_ * number_of_samples_in_one_window_ / sample_frequency_ << "s" << " - ";
  std::cout << "File Pos: " << file_pos - window_size_in_bytes_ << " - ";
  std::cout << "Size: " << window_size_in_bytes_ << std::endl;
}


int64_t UDPDataSource::GetFileSize(const char* file_name)
{

  struct __stat64 st;
  if (_stat64(file_name, &st) == 0)
    return st.st_size;
  return -1;
}

//Have to be replaced with AVX 
void UDPDataSource::ReverseInt16Arr(int16_t *pArrInt16)
{

	switch (interrogator_usage_type_) {
	  case REDUNDANT:
		{
			int channel_number_of_one_fiber = (number_of_channels_in_one_sample_ / 2);
			for (int i = 0; i < channel_number_of_one_fiber; i++) {
				pTempArr_[channel_number_of_one_fiber- 1-i] = pArrInt16[channel_number_of_one_fiber + i];
			}
			memcpy((pArrInt16+channel_number_of_one_fiber), pTempArr_, sizeof(int16_t)*channel_number_of_one_fiber);
			break;
		}
	  case SEPERATE:
		{
			int channel_number_of_one_fiber = (number_of_channels_in_one_sample_ / 2);
			for (int i = 0; i < channel_number_of_one_fiber; i++) {
				pTempArr_[channel_number_of_one_fiber - 1 - i] = pArrInt16[i];
			}
			memcpy(pArrInt16, pTempArr_, sizeof(int16_t)*channel_number_of_one_fiber);
			break;
		}
		default:
			break;
	}

}




