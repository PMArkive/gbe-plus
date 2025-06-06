// GB Enhanced+ Copyright Daniel Baxter 2022
// Licensed under the GPLv2
// See LICENSE.txt for full license text

// File : campho.cpp
// Date : December 28, 2022
// Description : Campho Advance
//
// Handles I/O for the Campho Advance (CAM-001)

#include "mmu.h"
#include "common/util.h"

/****** Resets Campho data structure ******/
void AGB_MMU::campho_reset()
{
	campho.data.clear();
	campho.g_stream.clear();

	campho.out_stream.clear();
	campho.config_data.clear();
	campho.config_data.resize(0x1C, 0x00);
	campho.contact_data.clear();
	campho.read_out_stream = false;
	campho.out_stream_index = 0;

	campho.bank_index_lo = 0;
	campho.bank_index_hi = 0;
	campho.bank_id = 0;
	campho.rom_stat = 0xA00A;
	campho.rom_cnt = 0;
	campho.block_len = 0;
	campho.block_stat = 0;
	campho.stream_started = false;
	campho.last_id = 0xFFFFFFFF;

	campho.video_capture_counter = 0;
	campho.video_frame_slice = 0;
	campho.video_frame_index = 0;
	campho.video_frame_size = 0;
	campho.capture_video = false;
	campho.new_frame = false;
	campho.is_large_frame = true;
	campho.update_local_camera = true;
	campho.update_remote_camera = false;
	campho.camera_mode = 0;
	campho.image_flip = false;

	campho.is_call_incoming = false;
	campho.is_call_active = false;
	campho.tele_data.clear();
	campho.tele_data_index = 0;
	campho.call_state = 0;

	campho.dialed_number = "";
	campho.at_command = "";

	campho.last_slice = 0;
	campho.repeated_slices = 0;

	campho.contact_index = -1;

	campho_read_contact_list();

	//Setup Campho networking
	//Note that all networking done here is completely separate from SIO
	campho.network_init = false;

	if(config::cart_type == AGB_CAMPHO)
	{
		campho_reset_network();
	}

	#ifdef GBE_NETPLAY

	campho.line.connected = false;
	campho.line.host_init = false;
	campho.line.remote_init = false;

	campho.ringer.connected = false;
	campho.ringer.host_init = false;
	campho.ringer.remote_init = false;

	campho.web.connected = false;
	campho.web.remote_init = false;

	#endif

	//Set default config data
	//Sensible values (50%) for settings like camera brightness/contrast and volumes
	//Data gets overwritten by a valid save file from MMU, if it exists
	campho.config_data[0] = 0x31;
	campho.config_data[1] = 0x08;
	campho.config_data[2] = 0x03;
	campho.config_data[3] = 0x00;

	campho.config_data[4] = 0xFE;
	campho.config_data[5] = 0x1F;
	campho.config_data[6] = 0xFF;
	campho.config_data[7] = 0xFF;


	campho.config_data[8] = 0x06;
	campho.config_data[9] = 0x40;


	campho.config_data[10] = 0x06;
	campho.config_data[11] = 0x40;


	campho.config_data[12] = 0x06;
	campho.config_data[13] = 0x40;


	campho.config_data[14] = 0x06;
	campho.config_data[15] = 0x40;

	campho.speaker_volume = 50;
	campho.mic_volume = 50;
	campho.video_brightness = 50;
	campho.video_contrast = 50;
}

/****** Writes data to Campho I/O ******/
void AGB_MMU::write_campho(u32 address, u8 value)
{
	switch(address)
	{
		//Campho ROM Status
		case CAM_ROM_STAT:
		case CAM_ROM_STAT_B:
			campho.rom_stat &= 0xFF00;
			campho.rom_stat |= value;
			break;

		case CAM_ROM_STAT+1:
		case CAM_ROM_STAT_B+1:
			campho.rom_stat &= 0xFF;
			campho.rom_stat |= (value << 8);

			//Process commands to read Graphics ROM
			if(campho.rom_stat == 0x4015)
			{
				campho.stream_started = true;
			}

			break;

		//Campho ROM Control
		case CAM_ROM_CNT:
		case CAM_ROM_CNT_B:
			campho.rom_cnt &= 0xFF00;
			campho.rom_cnt |= value;
			break;

		case CAM_ROM_CNT+1:
			campho.rom_cnt &= 0xFF;
			campho.rom_cnt |= (value << 8);

			//Detect access to main Program ROM
			if((campho.rom_cnt == 0xA00A) && (campho.block_stat < 0xCD00))
			{
				//Detect reading of first Program ROM bank
				if(!campho.block_stat)
				{
					campho.block_stat = 0xCC00;

					std::cout<<"MMU::Campho Reading PROM Bank 0x" << campho.block_stat << "\n";
					u32 prom_bank_id = campho_get_bank_by_id(campho.block_stat);
					campho_set_rom_bank(campho.mapped_bank_id[prom_bank_id], campho.mapped_bank_index[prom_bank_id], false);
				}

				//Increment Program ROM bank
				else
				{
					campho.block_stat++;

					//Signal end of Program ROM banks
					if(campho.block_stat == 0xCC10)
					{
						campho.block_stat = 0xCD00;
					}

					std::cout<<"MMU::Campho Reading PROM Bank 0x" << campho.block_stat << "\n";
					u32 prom_bank_id = campho_get_bank_by_id(campho.block_stat);
					campho_set_rom_bank(campho.mapped_bank_id[prom_bank_id], campho.mapped_bank_index[prom_bank_id], false);
				}
			}

			break;

		//Process Graphics ROM read requests
		case CAM_ROM_CNT_B+1:
			campho.rom_cnt &= 0xFF;
			campho.rom_cnt |= (value << 8);

			//Perform certain actions based on data from input stream (load graphics, camera commands, read/write settings)
			if(campho.rom_cnt == 0x4015)
			{
				campho_process_input_stream();
			}

			//Read next part of video camera framebuffer
			if((campho.rom_cnt == 0xA00A) && (campho.new_frame))
			{
				campho.video_frame_slice++;

				if(campho.camera_mode & 0x04) { campho_set_remote_video_data(); }
				else if(campho.camera_mode & 0x01) { campho_set_local_video_data(); }

				campho.last_slice = campho.video_frame_slice;
				campho.repeated_slices = 0;
			}

			//Continue processing active phone call data
			else if((campho.rom_cnt == 0xA00A) && (campho.call_state))
			{
				campho_process_call();
			}

			break;

		//Graphics ROM Stream
		case CAM_ROM_DATA_HI_B:
		case CAM_ROM_DATA_HI_B+1:
			if(campho.stream_started) { campho.g_stream.push_back(value); }
			break;
	}

	//std::cout<<"CAMPHO WRITE 0x" << address << " :: 0x" << (u32)value << "\n";
}

/****** Reads data from Campho I/O ******/
u8 AGB_MMU::read_campho(u32 address)
{
	u8 result = 0;

	switch(address)
	{	
		//Campho ROM Status
		case CAM_ROM_STAT:
		case CAM_ROM_STAT_B:
			result = (campho.rom_stat & 0xFF);
			break;

		case CAM_ROM_STAT+1:
		case CAM_ROM_STAT_B+1:
			result = ((campho.rom_stat >> 8) & 0xFF);
			break;

		//Campho ROM Control
		case CAM_ROM_CNT:
		case CAM_ROM_CNT_B:
			result = (campho.rom_cnt & 0xFF);
			break;

		case CAM_ROM_CNT+1:
		case CAM_ROM_CNT_B+1:
			result = ((campho.rom_cnt >> 8) & 0xFF);
			break;

		//Sequential ROM read
		default:
			result = read_campho_seq(address);
	}

	//std::cout<<"CAMPHO READ 0x" << address << " :: 0x" << (u32)result << "\n";
	return result;
}

/****** Reads sequential ROM data from Campho ******/
u8 AGB_MMU::read_campho_seq(u32 address)
{
	u8 result = 0;

	switch(address >> 24)
	{
		case 0xA:
		case 0xB:
			address -= 0x2000000;
			break;

		case 0xC:
		case 0xD:
			address -= 0x4000000;
			break;
	}

	//Read Low ROM Data Stream
	if((address < 0x8008000) || (address >= 0x8020000))
	{
		//Camera Video Data
		if(campho.new_frame)
		{
			if(campho.video_frame_index < campho.video_frame.size())
			{
				result = campho.video_frame[campho.video_frame_index++];
			}
		}

		//Telephony Data
		else if((campho.is_call_active) || (campho.call_state == 8))
		{
			if(campho.tele_data_index < campho.tele_data.size())
			{
				result = campho.tele_data[campho.tele_data_index++];
			}
		}

		else if(campho.bank_index_lo < campho.data.size())
		{
			result = campho.data[campho.bank_index_lo++];
		}
	}

	//Read High ROM Data Stream, Camera Video Data, or Campho Config Data
	else
	{
		//Misc Campho Data - Config Settings, Command Data, etc
		if(campho.read_out_stream)
		{
			if(campho.out_stream_index < campho.out_stream.size())
			{
				result = campho.out_stream[campho.out_stream_index++];
			}
		}

		//ROM
		else
		{
			if(campho.bank_index_hi < campho.data.size())
			{
				result = campho.data[campho.bank_index_hi++];
			}
		}
	}

	return result;
}

/****** Handles processing Campho input stream for commands ******/
void AGB_MMU::campho_process_input_stream()
{
	campho.stream_started = false;
	campho.read_out_stream = false;

	u16 header = (campho.g_stream[0] | (campho.g_stream[1] << 8));

	//Determine action based on stream size
	if((!campho.g_stream.empty()) && (campho.g_stream.size() >= 4))
	{
		u32 pos = campho.g_stream.size() - 4;
		u32 index = (campho.g_stream[pos] | (campho.g_stream[pos+1] << 8) | (campho.g_stream[pos+2] << 16) | (campho.g_stream[pos+3] << 24));
		u16 param_1 = header;

		//Send AT Command
		if(header == CAMPHO_SEND_AT_COMMAND)
		{
			u16 cmd_len = (campho.g_stream[2] | (campho.g_stream[3] << 8));
			cmd_len = ((cmd_len >> 13) | (cmd_len << 3));

			campho.at_command = "";

			//Grab AT command
			for(u32 x = 0, char_index = 4; x < cmd_len; x++)
			{
				u16 val = (campho.g_stream[char_index] | (campho.g_stream[char_index + 1] << 8));
				val = ((val >> 13) | (val << 3));

				//Even Characters
				if(x & 0x01)
				{
					u8 chr_val = ((val >> 8) & 0xFF);
					if(chr_val == 0) { break; }

					campho.at_command += ((val >> 8) & 0xFF);
					char_index += 2;
				}

				//Odd Characters
				else
				{
					u8 chr_val = (val & 0xFF);
					if(chr_val == 0) { break; }

					campho.at_command += (val & 0xFF);
				}
			}

			campho.tele_data.clear();
			campho.tele_data_index = 0;
			campho.call_state = 7;

			std::string at_response = "OK";
			u16 transfer_type = 0x0834;
			u16 at_size = ((at_response.size() << 13) | (at_response.size() >> 3));


			campho.tele_data.push_back(transfer_type & 0xFF);
			campho.tele_data.push_back((transfer_type >> 8) & 0xFF);
			campho.tele_data.push_back(at_size & 0xFF);
			campho.tele_data.push_back((at_size >> 8) & 0xFF);

			if(at_response.size() & 0x1)
			{
				u8 pad = 0x00;
				at_response += pad;
			}

			for(u32 x = 0; x < at_response.size(); x += 2)
			{
				u16 at_chr = (at_response[x] | (at_response[x + 1] << 8));
				at_chr = ((at_chr << 13) | (at_chr >> 3));

				campho.tele_data.push_back(at_chr & 0xFF);
				campho.tele_data.push_back(at_chr >> 8);
			}

			std::cout<<"AT Command Received: " << campho.at_command << "\n";
		}

		//Dial Phone Number
		else if(header == CAMPHO_DIAL_PHONE_NUMBER)
		{
			u16 number_len = (campho.g_stream[2] | (campho.g_stream[3] << 8));
			number_len = ((number_len >> 13) | (number_len << 3));
			if(number_len > 10) { number_len = 10; }
			
			campho.dialed_number = "";
			campho.network_state = 0x80;

			//Grab dialed number
			for(u32 x = 0, digit_index = 4; x < number_len; x++)
			{
				u16 val = (campho.g_stream[digit_index] | (campho.g_stream[digit_index + 1] << 8));
				val = ((val >> 13) | (val << 3));

				//Even Digits
				if(x & 0x01)
				{
					u8 chr_val = ((val >> 8) & 0xFF);
					if(chr_val == 0) { break; }

					campho.dialed_number += ((val >> 8) & 0xFF);
					digit_index += 2;
				}

				//Odd Digits
				else
				{
					u8 chr_val = (val & 0xFF);
					if(chr_val == 0) { break; }

					campho.dialed_number += (val & 0xFF);
				}
			}

			std::cout<<"Dialed Phone Number: " << campho.dialed_number << "\n";
		}

		//Grab Graphics ROM data
		else if(campho.g_stream.size() == 0x0C)
		{
			u32 param_2 = (campho.g_stream[4] | (campho.g_stream[5] << 8) | (campho.g_stream[6] << 16) | (campho.g_stream[7] << 24));
			campho.last_id = param_2;

			//Set new Graphics ROM bank
			u32 g_bank_id = campho_get_bank_by_id(param_2, index);

			if(g_bank_id != 0xFFFFFFFF)
			{
				campho_set_rom_bank(campho.mapped_bank_id[g_bank_id], campho.mapped_bank_index[g_bank_id], true);
			}

			else
			{
				std::cout<<"Unknown Graphics ID -> 0x" << param_2 << " :: " << index << "\n";
			}

			campho.video_capture_counter = 0;
			campho.new_frame = false;
			campho.video_frame_slice = 0;
			campho.last_slice = 0;

			std::cout<<"Graphics ROM ID -> 0x" << param_2 << "\n";
			std::cout<<"Graphics ROM Index -> 0x" << index << "\n";
		}

		//Campho commands
		else if(campho.g_stream.size() == 0x04)
		{
			//Stop camera?
			if(index == CAMPHO_STOP_CAMERA)
			{
				campho.capture_video = false;

				campho.video_capture_counter = 0;
				campho.new_frame = false;
				campho.video_frame_slice = 0;
				campho.last_slice = 0;
				campho.camera_mode = 0;
				campho.update_local_camera = false;
				campho.update_remote_camera = false;
			}

			//Turn on camera for large frame?
			else if(index == CAMPHO_GET_CAMERA_FRAME_LARGE)
			{
				campho.capture_video = true;
				campho.is_large_frame = true;
				campho.update_local_camera = true;

				//Large video frame = 176x144, drawn 12 lines at a time
				campho.video_frame_size = 176 * 12;

				campho.video_capture_counter = 0;
				campho.new_frame = false;
				campho.video_frame_slice = 0;
				campho.last_slice = 0;
			}

			//Read the number of contact data entries
			else if(index == CAMPHO_GET_CONFIG_ENTRY_COUNT)
			{
				campho.out_stream.clear();

				//Metadata for Campho Advance
				campho.out_stream.push_back(0x32);
				campho.out_stream.push_back(0x08);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x40);

				u16 num_of_contacts = campho.contact_data.size() / 28;
				num_of_contacts = ((num_of_contacts << 13) | (num_of_contacts >> 3));

				campho.out_stream.push_back(num_of_contacts & 0xFF);
				campho.out_stream.push_back(num_of_contacts >> 8);

				//Allow outstream to be read (until next stream)
				campho.out_stream_index = 0;
				campho.read_out_stream = true;
			}

			//Turn on camera for small frame?
			else if(index == CAMPHO_GET_CAMERA_FRAME_SMALL)
			{
				campho.capture_video = true;
				campho.is_large_frame = false;
				campho.update_local_camera = true;

				//Small video frame = 58x48, drawn 35 and 13 lines at a time
				campho.video_frame_size = 58 * 35;

				campho.video_capture_counter = 0;
				campho.new_frame = false;
				campho.video_frame_slice = 0;
				campho.last_slice = 0;
			}

			//Always end frame rendering
			else if(index == CAMPHO_FINISH_CAMERA_FRAME)
			{
				campho.video_capture_counter = 0;
				campho.new_frame = false;
				campho.video_frame_slice = 0;
				campho.last_slice = 0;
			}

			//Answer phone call
			else if(index == CAMPHO_ANSWER_PHONE_CALL)
			{
				campho.call_state = 3;
				campho.network_state = 4;
			}

			//End call
			else if(index == CAMPHO_END_PHONE_CALL)
			{
				campho.call_state = 5;
				campho.network_state = 0xFE;
			}

			//Cancel a phone call before connecting
			else if(index == CAMPHO_CANCEL_PHONE_CALL)
			{
				campho.network_state = 0xFF;
			}

			//Flip camera image
			else if(index == CAMPHO_SET_VIDEO_FLIP)
			{
				campho.image_flip = !campho.image_flip;
				campho.read_out_stream = true;
				campho.out_stream.clear();
			}

			std::cout<<"Camera Command -> 0x" << index << "\n";
		}

		//Change Campho settings
		else if(campho.g_stream.size() == 0x06)
		{
			u16 stream_stat = (campho.g_stream[1] << 8) | campho.g_stream[0];
			u16 hi_set = (index & 0xFFFF0000) >> 16;
			u16 lo_set = (index & 0xFFFF);

			//Read full settings
			if(stream_stat == CAMPHO_READ_CONFIG_DATA)
			{
				campho.out_stream.clear();

				//Set data to read from stream
				if(index == 0x1FFE4000)
				{
					for(u32 x = 0; x < campho.config_data.size(); x++)
					{
						campho.out_stream.push_back(campho.config_data[x]);
					}
				}

				else if((index & 0xFFFF) == 0x4000)
				{

					//Reset contact index
					if((index == 0xFFFF4000) || (index == 0x00004000))
					{
						campho.contact_index = 0;
					}

					//Set new contact index
					else
					{
						u16 access_param = (index >> 16);
						access_param = ((access_param >> 13) | (access_param << 3));

						//Get new contact index when scrolling down list
						if((access_param & 0xFF) == 0x01)
						{
							campho.contact_index = 1 + (access_param >> 8);
						}

						//Get new contact index when scrolling back up list
						else if(((access_param & 0xFF) == 0xFF) || ((access_param & 0xFF) == 0x00))
						{
							campho.contact_index = (access_param >> 8);
						}
					}

					u32 max_size = (campho.contact_data.size() / 28);

					if((campho.contact_index < 0) || (campho.contact_index >= max_size))
					{
						campho.contact_index = 0;
					}

					if(max_size)
					{
						u32 offset = campho.contact_index * 28;

						for(u32 x = 0; x < 28; x++)
						{
							campho.out_stream.push_back(campho.contact_data[offset + x]);
						}

						u16 index_bytes = campho.contact_index * 0x20;

						campho.out_stream[4] = (index_bytes & 0xFF);
						campho.out_stream[5] = (index_bytes >> 8);
					}

					else { campho.out_stream.push_back(0x00); }
				}
			}

			//Set microphone volume
			else if(stream_stat == CAMPHO_SET_MIC_VOLUME)
			{
				campho.mic_volume = campho_find_settings_val(hi_set);
				u32 read_data = (campho_convert_settings_val(campho.mic_volume) << 16) | 0x4000;
				campho_make_settings_stream(read_data);
			}

			//Set speaker volume
			else if(stream_stat == CAMPHO_SET_SPEAKER_VOLUME)
			{
				campho.speaker_volume = campho_find_settings_val(hi_set);
				u32 read_data = (campho_convert_settings_val(campho.speaker_volume) << 16) | 0x4000;
				campho_make_settings_stream(read_data);
			}

			//Set video brightness
			else if(stream_stat == CAMPHO_SET_VIDEO_BRIGHTNESS)
			{
				campho.video_brightness = campho_find_settings_val(hi_set);
				u32 read_data = (campho_convert_settings_val(campho.video_brightness) << 16) | 0x4000;
				campho_make_settings_stream(read_data);
				campho.update_local_camera = true;
			}

			//Set video contrast
			else if(stream_stat == CAMPHO_SET_VIDEO_CONTRAST)
			{
				campho.video_contrast = campho_find_settings_val(hi_set);
				u32 read_data = (campho_convert_settings_val(campho.video_contrast) << 16) | 0x4000;
				campho_make_settings_stream(read_data);
				campho.update_local_camera = true;
			}

			//Erase contact data
			else if(stream_stat == CAMPHO_ERASE_CONTACT_DATA)
			{
				u16 temp_index = (index >> 16);
				temp_index = ((temp_index >> 13) | (temp_index << 3));
				temp_index >>= 8;

				u32 del_start = (temp_index * 28);
				u32 del_end = (del_start + 28);

				if(del_end <= campho.contact_data.size())
				{
					campho.contact_data.erase(campho.contact_data.begin() + del_start, campho.contact_data.begin() + del_end);
				} 
			}

			//Allow settings to be read now (until next stream)
			campho.out_stream_index = 0;
			campho.read_out_stream = true;

			campho.video_capture_counter = 0;
			campho.new_frame = false;
			campho.video_frame_slice = 0;
			campho.last_slice = 0;

			std::cout<<"Campho Settings -> 0x" << index << " :: 0x" << stream_stat << "\n";
		}

		//Save Campho settings changes
		else if(campho.g_stream.size() == 0x1C)
		{
			u32 sub_header = (campho.g_stream[4] | (campho.g_stream[5] << 8) | (campho.g_stream[6] << 16) | (campho.g_stream[7] << 24));

			//Save configuration settings
			if(sub_header == 0xFFFF1FFE)
			{
				campho.config_data.clear();

				//32-bit metadata
				campho.config_data.push_back(0x31);
				campho.config_data.push_back(0x08);
				campho.config_data.push_back(0x03);
				campho.config_data.push_back(0x00);

				for(u32 x = 4; x < 0x1C; x++) { campho.config_data.push_back(campho.g_stream[x]); }

				//Process Image Flip now. Now dedicated command for this like the others?
				bool old_flag = campho.image_flip;
				campho.image_flip = ((campho.g_stream[0x11] << 8) | campho.g_stream[0x10]) ? true : false;
				campho.update_local_camera = true;

				//Allow settings to be read now (until next stream)
				campho.out_stream_index = 0;
				campho.read_out_stream = true;

				std::cout<<"Campho Config Saved\n";
			}

			//Save Name + Phone Number
			else if((sub_header & 0xFFFF) == 0xFFFF)
			{
				//32-bit metadata
				campho.contact_data.push_back(0x31);
				campho.contact_data.push_back(0x08);
				campho.contact_data.push_back(0x03);
				campho.contact_data.push_back(0x00);

				for(u32 x = 4; x < 0x1C; x++) { campho.contact_data.push_back(campho.g_stream[x]); }

				//Allow outstream to be read (until next stream)
				campho.out_stream_index = 0;
				campho.read_out_stream = true;

				std::string contact_name = campho_convert_contact_name();
				std::string contact_number = "";

				//Parse incoming data for contact phone number (last 10 bytes of stream)
				for(u32 x = 0, digit_index = 18; x < 10; x++)
				{
					u16 val = (campho.g_stream[digit_index] | (campho.g_stream[digit_index + 1] << 8));
					val = ((val >> 13) | (val << 3));

					//Even Digits
					if(x & 0x01)
					{
						u8 chr_val = ((val >> 8) & 0xFF);
						if(chr_val == 0) { break; }

						contact_number += ((val >> 8) & 0xFF);
						digit_index += 2;
					}

					//Odd Digits
					else
					{
						u8 chr_val = (val & 0xFF);
						if(chr_val == 0) { break; }

						contact_number += (val & 0xFF);
					}
				}

				std::cout<<"Contact Name: " << contact_name << "\n";
				std::cout<<"Contact Number: " << contact_number << "\n";
				std::cout<<"Campho Added Contact\n";
			}

			//Edit and update existing Name + Phone Number
			else
			{
				s32 edit_index = campho.contact_index * 28;

				//32-bit metadata
				campho.contact_data[edit_index++] = 0x31;
				campho.contact_data[edit_index++] = 0x08;
				campho.contact_data[edit_index++] = 0x03;
				campho.contact_data[edit_index++] = 0x00;
				campho.contact_data[edit_index++] = 0xFF;
				campho.contact_data[edit_index++] = 0xFF;
				campho.contact_data[edit_index++] = 0x00;
				campho.contact_data[edit_index++] = 0x00;

				//Setup separate out stream vs actual contact data that is saved
				campho.out_stream.clear();
				edit_index = (campho.contact_index * 28) + 8;

				campho.out_stream.push_back(0x32);
				campho.out_stream.push_back(0x48);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x00);
				campho.out_stream.push_back(0x00);

				for(u32 x = 0x08; x < 0x1C; x++)
				{
					campho.out_stream.push_back(campho.contact_data[edit_index]);
					campho.contact_data[edit_index++] = campho.g_stream[x];
				}

				//Allow outstream to be read (until next stream)
				campho.out_stream_index = 0;
				campho.read_out_stream = true;

				std::string contact_name = campho_convert_contact_name();
				std::string contact_number = "";

				//Parse incoming data for contact phone number (last 10 bytes of stream)
				for(u32 x = 0, digit_index = 18; x < 10; x++)
				{
					u16 val = (campho.g_stream[digit_index] | (campho.g_stream[digit_index + 1] << 8));
					val = ((val >> 13) | (val << 3));

					//Even Digits
					if(x & 0x01)
					{
						u8 chr_val = ((val >> 8) & 0xFF);
						if(chr_val == 0) { break; }

						contact_number += ((val >> 8) & 0xFF);
						digit_index += 2;
					}

					//Odd Digits
					else
					{
						u8 chr_val = (val & 0xFF);
						if(chr_val == 0) { break; }

						contact_number += (val & 0xFF);
					}
				}

				std::cout<<"Contact Name: " << contact_name << "\n";
				std::cout<<"Contact Number: " << contact_number << "\n";
				std::cout<<"Campho Updated Contact\n";
			}

			campho.video_capture_counter = 0;
			campho.new_frame = false;
			campho.video_frame_slice = 0;
			campho.last_slice = 0;
		}

		else
		{
			std::cout<<"Unknown Campho Input. Size -> 0x" << campho.g_stream.size() << "\n";
		}
	}

	campho.g_stream.clear();
}

/****** Handles processing Campho call data when receiving/making a phone call ******/
void AGB_MMU::campho_process_call()
{
	//TODO - Use enums when the actual purpose of all known states has been researched
	switch(campho.call_state)
	{
		//Establish an incoming call
		case 1:
			campho.rom_stat = 0x4015;
			campho.tele_data.clear();
			campho.tele_data_index = 0;
			campho.call_state = 2;
			break;

		//End call
		case 6:
			campho.rom_stat = 0x4015;
			campho.tele_data.clear();
			campho.tele_data_index = 0;
			campho.call_state = 0;

			campho_close_network();
			campho_reset_network();

			break;

		//Finish AT command
		case 8:
			campho.rom_stat = 0x4015;
			campho.tele_data.clear();
			campho.tele_data_index = 0;
			campho.call_state = 0;

			break;
	}
}		

/****** Sets the absolute position within the Campho ROM for a bank's base address ******/
void AGB_MMU::campho_set_rom_bank(u32 bank, u32 address, bool set_hi_bank)
{
	//Abort if invalid bank is set
	if(bank == 0xFFFFFFFF) { return; }

	//Search all known banks for a specific ID
	for(u32 x = 0; x < campho.mapped_bank_id.size(); x++)
	{
		//Match bank ID and base address
		if((campho.mapped_bank_id[x] == bank) && (campho.mapped_bank_index[x] == address))
		{
			//Set High ROM bank
			if(set_hi_bank) { campho.bank_index_hi = campho.mapped_bank_pos[x]; }

			//Set Low ROM bank
			else { campho.bank_index_lo = campho.mapped_bank_pos[x]; }

			campho.block_len = campho.mapped_bank_len[x];
			return;
		}
	}

	std::cout<<"MMU::Warning - Campho Advance ROM position for Bank 0x" << bank << " @ 0x" << address << " was not found\n";
}

/****** Maps various ROM banks for the Campho ******/
void AGB_MMU::campho_map_rom_banks()
{
	u32 rom_pos = 0;
	u32 bank_id = 0x03;
	u32 bank_index = 0x00;

	campho.mapped_bank_id.clear();
	campho.mapped_bank_index.clear();
	campho.mapped_bank_len.clear();
	campho.mapped_bank_pos.clear();

	//Setup BS1 and BS2
	campho.mapped_bank_id.push_back(0x08000000);
	campho.mapped_bank_index.push_back(0x00);
	campho.mapped_bank_len.push_back(0x159);
	campho.mapped_bank_pos.push_back(rom_pos);
	rom_pos += 0x159;

	campho.mapped_bank_id.push_back(0x08008000);
	campho.mapped_bank_index.push_back(0x00);
	campho.mapped_bank_len.push_back(0x84);
	campho.mapped_bank_pos.push_back(rom_pos);
	rom_pos += 0x84;

	//Setup Program ROM
	for(u32 x = 0; x < 16; x++)
	{
		campho.mapped_bank_id.push_back(0xCC00 + x);
		campho.mapped_bank_index.push_back(0x00);
		campho.mapped_bank_len.push_back(0xFFE);
		campho.mapped_bank_pos.push_back(rom_pos);
		rom_pos += 0xFFE;
	}

	campho.mapped_bank_id.push_back(0xCD00);
	campho.mapped_bank_index.push_back(0x00);
	campho.mapped_bank_len.push_back(0x04);
	campho.mapped_bank_pos.push_back(rom_pos);
	rom_pos += 0x04;

	//Setup Graphics ROM
	while(bank_id < 320)
	{
		u16 real_id = ((bank_id << 13) | (bank_id >> 3));

		//Setup 0xFFFFFFFF bank
		campho.mapped_bank_id.push_back(real_id);
		campho.mapped_bank_index.push_back(0xFFFFFFFF);
		campho.mapped_bank_len.push_back(0x10);
		campho.mapped_bank_pos.push_back(rom_pos);

		//Read overall length of bank
		u32 l_index = rom_pos + 0x0C;
		u32 total_len = ((campho.data[l_index + 3] << 24) | (campho.data[l_index + 2] << 16) | (campho.data[l_index + 1] << 8) | (campho.data[l_index]));

		if(total_len > 0x2000) { total_len = (total_len & 0xFFFF) + (total_len >> 16); }

		rom_pos += 0x10;
		bank_index = 0x00;

		//Setup all banks under this current ID
		while(total_len)
		{
			l_index = rom_pos + 0x02;
			u32 current_len = ((campho.data[l_index + 1] << 8) | (campho.data[l_index]));
			total_len -= current_len;
			current_len *= 8;

			campho.mapped_bank_id.push_back(real_id);
			campho.mapped_bank_index.push_back(bank_index);
			campho.mapped_bank_len.push_back(current_len);
			campho.mapped_bank_pos.push_back(rom_pos);
			
			bank_index += 0x1F4;

			rom_pos += current_len;
			rom_pos += 4;
		}

		bank_id++;

		//Skip the following banks
		//Some generate I/O errors when reading and are not part of the Campho Advance
		//Others are empty banks (zero-length) and are also not part of the Campho Advance dump
		if(bank_id == 93) { bank_id++; }
		else if(bank_id == 315) { bank_id += 2; }
	}

	//Use initial BS1 and BS2 banks
	u32 bs1_bank = campho_get_bank_by_id(0x08000000);
	u32 bs2_bank = campho_get_bank_by_id(0x08008000);
	
	campho_set_rom_bank(campho.mapped_bank_id[bs1_bank], campho.mapped_bank_index[bs1_bank], false);
	campho_set_rom_bank(campho.mapped_bank_id[bs2_bank], campho.mapped_bank_index[bs2_bank], true);

	//When not using the GBA BIOS, ignore the first 281 ROM reads done by the BIOS
	if(!config::use_bios)
	{
		campho.bank_index_lo = 0x111;
		campho.bank_index_hi = 0x161;
	}
}

/****** Returns the internal ROM bank GBE+ needs - Mapped to the Campho Advance's IDs ******/
u32 AGB_MMU::campho_get_bank_by_id(u32 id)
{
	for(u32 x = 0; x < campho.mapped_bank_id.size(); x++)
	{
		if(campho.mapped_bank_id[x] == id) { return x; }
	}

	return 0xFFFFFFFF;
}

/****** Returns the internal ROM bank GBE+ needs - Mapped to the Campho Advance's IDs ******/
u32 AGB_MMU::campho_get_bank_by_id(u32 id, u32 index)
{
	if((index != 0xFFFFFFFF) && (index & 0xF0000000))
	{
		index = (index & 0xFFFF) + (index >> 16);
	}

	for(u32 x = 0; x < campho.mapped_bank_id.size(); x++)
	{
		if((campho.mapped_bank_id[x] == id) && (campho.mapped_bank_index[x] == index)) { return x; }
	}

	return 0xFFFFFFFF;
}

/****** Processes regular events such as audio/video capture and telephony for the Campho Advance ******/
void AGB_MMU::process_campho()
{
	//Prioritize Campho Networking first!
	campho_process_networking();

	//Initiate a live phone call -> Alert the Campho *receiving* a call
	if((campho.is_call_incoming) && (!campho.is_call_active) && (campho.call_state == 0) && (!campho.new_frame))
	{
		campho.is_call_active = true;
		campho.call_state = 1;
		campho.rom_stat = 0xA00A;

		campho.tele_data.clear();
		campho.tele_data_index = 0;

		//Set telephone data stream for accepting a live phone call
		//16-bit units, 1 - 0xAB00, 2 = Data Length, 3 = ???, 4 = Phone Status Flag
		campho.tele_data.push_back(0x60);
		campho.tele_data.push_back(0x15);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x80);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x04);
		campho.tele_data.push_back(0x00);

		return;
	}

	//Answer a live phone call -> Answer the phone call as the Campho *receiving* a call
	else if((campho.is_call_active) && (campho.call_state == 3) && (!campho.new_frame))
	{
		campho.call_state = 4;
		campho.rom_stat = 0xA00A;

		campho.tele_data.clear();
		campho.tele_data_index = 0;

		//Set telephone data stream for accepting a live phone call
		//16-bit units, 1 - 0xAB00, 2 = Data Length, 3 = ???, 4 = Phone Status Flag
		campho.tele_data.push_back(0x60);
		campho.tele_data.push_back(0x15);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x80);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0xA0);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x00);
	}

	//End current phone call
	else if((campho.is_call_active) && (campho.call_state == 5) && (!campho.new_frame))
	{
		campho.call_state = 6;
		campho.rom_stat = 0xA00A;

		campho.tele_data.clear();
		campho.tele_data_index = 0;

		//Set telephone data stream for accepting a live phone call
		//16-bit units, 1 - 0xAB00, 2 = Data Length, 3 = ???, 4 = Phone Status Flag
		campho.tele_data.push_back(0x60);
		campho.tele_data.push_back(0x15);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x80);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x00);
		campho.tele_data.push_back(0x00);

		return;
	}

	//Send AT command
	else if(campho.call_state == 7)
	{
		campho.call_state = 8;
		campho.rom_stat = 0xA00A;

		return;
	}

	campho.video_capture_counter++;

	if(campho.video_capture_counter < 12)
	{
		return;
	}

	else { campho.video_capture_counter = 0; }

	//Abort/Finish video rendering midframe if delayed by other I/O like ROM
	if((campho.capture_video) && (campho.last_slice == campho.video_frame_slice))
	{
		campho.repeated_slices++;

		if(campho.repeated_slices == 2)
		{
			campho.rom_stat = 0x4015;
			campho.last_slice = 0;
			campho.repeated_slices = 0;
			campho.video_capture_counter = 0;
			campho.new_frame = false;
			campho.video_frame_slice = 0;

			return;
		}
	}

	//Update video capture data with new pixels for current frame - Update at ~5FPS
	if((campho.capture_video) && (!campho.new_frame))
	{
		campho.new_frame = true;
		campho.video_frame_slice = 0;
		campho.last_slice = 0;
		campho.rom_stat = 0xA00A;

		//Grab pixel data for captured video frame
		//Pull data from BMP file or webcam
		if(campho.update_local_camera)
		{
			//Use webcam data if available
			if(!campho.web_cam_img.empty())
			{
				campho_get_image_data(campho.web_cam_img.data(), campho.local_capture_buffer, 176, 144, false);
			} 
			
			//Use static BMP
			else
			{
				SDL_Surface* source = SDL_LoadBMP(config::external_camera_file.c_str());

				if(source != NULL)
				{
					u8* cam_pixel_data = (u8*)source->pixels;
					campho_get_image_data(cam_pixel_data, campho.local_capture_buffer, source->w, source->h, false);	
				}

				SDL_FreeSurface(source);
			}

			campho_set_local_video_data();
			campho.update_local_camera = false;
			campho.camera_mode |= 0x01;
		}

		//Flag a remote video frame as ready to draw
		//Data previously pulled from network buffer
		if(campho.update_remote_camera)
		{			
			campho.update_remote_camera = false;
			campho.camera_mode |= 0x02;
		}

		if(campho.camera_mode & 0x04) { campho_set_remote_video_data(); }
		else if(campho.camera_mode & 0x01) { campho_set_local_video_data(); }

		return;
	}
}

/****** Sets the framebuffer data for Campho's video input - Local Version ******/
void AGB_MMU::campho_set_local_video_data()
{
	//Setup new frame data
	campho.video_frame.clear();
	campho.video_frame_index = 0;
	u16 frame_msb = (campho.is_large_frame) ? 0xAA00 : 0xA900;

	//2-byte metadata, position and size of frame
	u16 pos = frame_msb + campho.video_frame_slice;
	pos &= (campho.is_large_frame) ? 0xFFFF : 0xFF01;
	pos = ((pos >> 3) | (pos << 13));

	u16 v_size = campho.video_frame_size * 2;
	u8 line_size = (campho.is_large_frame) ? 176 : 58;

	u8 slice_limit_prep = campho.is_large_frame ? 13 : 2;
	u8 slice_limit_end = campho.is_large_frame ? 14 : 3;

	//Switch video size from 58*35 to 58*13 for 2nd part of small video frame rendering
	//Normally okay to do 58*35 twice, except when changing settings (which draws garbage data)
	if((!campho.is_large_frame) && (campho.video_frame_slice == 1))
	{
		v_size = (58 * 13 * 2);
	}

	//Check whether the video frame has been fully rendered
	//In that case, set position and size to 0xCFFF and 0x00 respectively
	if(campho.video_frame_slice == slice_limit_prep)
	{
		pos = 0xF9FF;
		v_size = 0;
	}

	//Check whether 0xCFFF has been previously sent, end video frame rendering now
	//Forcing ROM_STAT to 0x4015 signals end all of video frame data from Campho
	else if(campho.video_frame_slice == slice_limit_end)
	{
		//Transfer user's current camera input over network
		if((campho.is_call_video_enabled) && (!campho.is_large_frame)) { campho.send_video_data = true; }

		campho.rom_stat = 0x4015;

		//If a remote video frame has been queued, draw it only after local video is done
		//Also, do this *immediately* afterwards instead of delaying 12 frames to achieve ~5PS
		if(campho.camera_mode & 0x02)
		{
			campho.camera_mode &= 0x01;
			campho.camera_mode |= 0x04;
			campho.video_capture_counter = 11;
		}	

		return;
	}

	v_size = ((v_size >> 3) | (v_size << 13));

	campho.video_frame.push_back(pos & 0xFF);
	campho.video_frame.push_back(pos >> 8);
	campho.video_frame.push_back(v_size & 0xFF);
	campho.video_frame.push_back(v_size >> 8);

	if(!v_size) { return; }

	u32 line_pos = campho.video_frame_slice;
	line_pos *= (campho.is_large_frame) ? 12 : 35;

	if(campho.is_large_frame)
	{
		if(campho.video_frame_slice != 0) { line_pos -= campho.video_frame_slice; }
	}

	line_pos *= (line_size * 2);

	for(u32 x = 0; x < campho.video_frame_size * 2; x++)
	{	
		if(line_pos < campho.local_capture_buffer.size())
		{
			campho.video_frame.push_back(campho.local_capture_buffer[line_pos++]);
		}
	}
}

/****** Sets the framebuffer data for Campho's video input - Remote Version ******/
void AGB_MMU::campho_set_remote_video_data()
{
	//Setup new frame data
	campho.video_frame.clear();
	campho.video_frame_index = 0;
	u16 frame_msb = 0xAA00;

	//2-byte metadata, position and size of frame
	u16 pos = frame_msb + campho.video_frame_slice;
	pos = ((pos >> 3) | (pos << 13));

	u16 f_size = (176 * 12);
	u16 v_size = f_size * 2;
	u8 line_size = 176;

	u8 slice_limit_prep = 13;
	u8 slice_limit_end = 14;

	//Check whether the video frame has been fully rendered
	//In that case, set position and size to 0xCFFF and 0x00 respectively
	if(campho.video_frame_slice == slice_limit_prep)
	{
		pos = 0xF9FF;
		v_size = 0;
	}

	//Check whether 0xCFFF has been previously sent, end video frame rendering now
	//Forcing ROM_STAT to 0x4015 signals end all of video frame data from Campho
	else if(campho.video_frame_slice == slice_limit_end)
	{
		campho.rom_stat = 0x4015;
		campho.camera_mode &= 0x01;
		return;
	}

	v_size = ((v_size >> 3) | (v_size << 13));

	campho.video_frame.push_back(pos & 0xFF);
	campho.video_frame.push_back(pos >> 8);
	campho.video_frame.push_back(v_size & 0xFF);
	campho.video_frame.push_back(v_size >> 8);

	if(!v_size) { return; }

	u32 line_pos = campho.video_frame_slice;
	line_pos *= 12;

	if(campho.video_frame_slice != 0) { line_pos -= campho.video_frame_slice; }

	line_pos *= (line_size * 2);

	for(u32 x = 0; x < f_size * 2; x++)
	{	
		if(line_pos < campho.remote_capture_buffer.size())
		{
			campho.video_frame.push_back(campho.remote_capture_buffer[line_pos++]);
		}
	}
}

/****** Converts 24-bit RGB data into 15-bit GBA colors for Campho video buffer ******/
void AGB_MMU::campho_get_image_data(u8* img_data, std::vector <u8> &out_buffer, u32 width, u32 height, bool is_net_video)
{
	u32 len = width * height;
	u32 data_index = 0;
	std::vector <u8> temp_buffer;

	u8 target_width = (campho.is_large_frame || is_net_video) ? 176 : 58;
	u8 target_height = (campho.is_large_frame || is_net_video) ? 144 : 48;

	//Grab original image data, scale later if necessary
	for(u32 x = 0; x < len; x++)
	{
		u8 r = (img_data[data_index + 2] >> 3);
		u8 g = (img_data[data_index + 1] >> 3);
		u8 b = (img_data[data_index] >> 3);

		u32 input_color = 0xFF000000 | (img_data[data_index + 2] << 16) | (img_data[data_index + 1] << 8) | img_data[data_index];

		//Adjust brightness as per user settings - Do nothing if settings are in the middle
		if(campho.video_brightness != 5)
		{
			double ratio = (campho.video_brightness - 5) / 5.0;

			r = (input_color >> 16);
			g = (input_color >> 8);
			b = input_color;

			s16 nr = r + s16(r * ratio);
			s16 ng = g + s16(g * ratio);
			s16 nb = b + s16(b * ratio);

			if(ratio > 0)
			{
				r = (nr > 255) ? 255 : nr;
				g = (ng > 255) ? 255 : ng;
				b = (nb > 255) ? 255 : nb;
			}

			else
			{
				r = (nr < 0) ? 0 : nr;
				g = (ng < 0) ? 0 : ng;
				b = (nb < 0) ? 0 : nb;
			}

			input_color = 0xFF000000 | (r << 16) | (g << 8) | b;

			r = (input_color >> 19) & 0x1F;
			g = (input_color >> 11) & 0x1F;
			b = (input_color >> 3) & 0x1F;
		}

		//Adjust contrast as per user settings - Do nothing if settings are in the middle
		if(campho.video_contrast != 5)
		{
			s16 ratio = -255 + (campho.video_contrast * 51);
			input_color = util::adjust_contrast(input_color, ratio);

			r = (input_color >> 19) & 0x1F;
			g = (input_color >> 11) & 0x1F;
			b = (input_color >> 3) & 0x1F;

		}

		u16 color = ((b << 10) | (g << 5) | r);
		color = ((color >> 3) | (color << 13));

		temp_buffer.push_back(color & 0xFF);
		temp_buffer.push_back(color >> 0x08);

		data_index += 3;
	}

	//Preserve 1st 4 bytes of buffer for video transmissions over network (used for header)
	//Otherwise, start from scratch
	if(is_net_video) { out_buffer.resize(4); }
	else { out_buffer.clear(); }

	//Calculate X and Y ratio for stretching/shrinking
	float x_ratio = float(width) / target_width;
	float y_ratio = float(height) / target_height;

	u32 x_pos = 0;
	u32 y_pos = 0;
	u32 pos = 0;

	u32 target_len = target_width * target_height;

	for(u32 x = 0; x < target_len; x++)
	{
		u32 x_pos = (x % target_width) * x_ratio;
		u32 y_pos = (x / target_width) * y_ratio;
		u32 pos = ((y_pos * width) + x_pos) * 2;

		if(pos < temp_buffer.size())
		{
			out_buffer.push_back(temp_buffer[pos]);
			out_buffer.push_back(temp_buffer[pos+1]);
		}
	}

	//Flip final output if necessary
	if(campho.image_flip)
	{
		u32 offset = (is_net_video) ? 4 : 0;

		for(u32 x = offset; x < out_buffer.size() / 2;)
		{
			u16 current_y = (x / 2) / target_width;
			u16 current_x = (x / 2) % target_width;

			u8 src_1 = out_buffer[x];
			u8 src_2 = out_buffer[x + 1];

			u32 dst_pos = ((((target_height - 1) - current_y) * target_width) + current_x) * 2;

			u8 dst_1 = out_buffer[dst_pos];
			u8 dst_2 = out_buffer[dst_pos + 1];

			out_buffer[x] = dst_1;
			out_buffer[x + 1] = dst_2;

			out_buffer[dst_pos] = src_1;
			out_buffer[dst_pos + 1] = src_2;

			x += 2;
		}
	}
}

/****** Finds the regular integer value of the settings the Campho Advance uses ******/
u8 AGB_MMU::campho_find_settings_val(u16 input)
{
	for(u32 x = 0; x <= 10; x++)
	{
		u32 test = ((x << 14) | x);
		test += ((test & 0xFFFF0000) >> 16);
		test &= 0xFFFF;

		if(input == test) { return x; }
	}

	return 0;
}

/****** Converts a regular integer value into the same format the Campho Advances uses for settings ******/
u16 AGB_MMU::campho_convert_settings_val(u8 input)
{
	u32 result = ((input << 14) | input);
	result += ((result & 0xFFFF0000) >> 16);
	result &= 0xFFFF;
	return result;
}

/****** Makes an 8-byte stream that returns the a current settings value when read by the Campho ******/
void AGB_MMU::campho_make_settings_stream(u32 input)
{
	campho.out_stream.clear();

	//Set data to read from stream
	campho.out_stream.push_back(0x20);
	campho.out_stream.push_back(0x68);
	campho.out_stream.push_back(input);
	campho.out_stream.push_back(input >> 8);
	campho.out_stream.push_back(input >> 16);
	campho.out_stream.push_back(input >> 24);
	campho.out_stream.push_back(0x00);
	campho.out_stream.push_back(0x60);
}

/****** Converts output data from stream into a Unicode string ******/
std::string AGB_MMU::campho_convert_contact_name()
{
	std::string result = "";

	for(u32 x = 0; x < 10; x += 2)
	{
		u16 chr = (campho.g_stream[x + 8] << 8) | campho.g_stream[x + 9];
		chr = ((chr >> 13) | (chr << 3));

		u8 hi_chr = (chr >> 8);
		u8 lo_chr = (chr & 0xFF);

		for(u32 y = 0; y < 2; y++)
		{
			std::string segment = "";
			u8 conv_chr = (y == 0) ? hi_chr : lo_chr;

			//Terminate string on NULL character
			if(conv_chr == 0x00) { return result; }

			//Handle ASCII characters
			else if(conv_chr < 0x80) { segment += conv_chr; }

			//Handle custom space character
			else if(conv_chr == 0xDA) { segment += " "; }

			//Handle katakana
			else if((conv_chr >= 0xA1) && (conv_chr <= 0xD9))
			{
				switch(conv_chr)
				{
					case 0xA1: segment += "\u30A2"; break;
					case 0xA2: segment += "\u30A4"; break;
					case 0xA3: segment += "\u30A6"; break;
					case 0xA4: segment += "\u30A8"; break;
					case 0xA5: segment += "\u30AA"; break;
					case 0xA6: segment += "\u30AB"; break;
					case 0xA7: segment += "\u30AD"; break;
					case 0xA8: segment += "\u30AF"; break;
					case 0xA9: segment += "\u30B1"; break;
					case 0xAA: segment += "\u30B3"; break;
					case 0xAB: segment += "\u30B5"; break;
					case 0xAC: segment += "\u30B7"; break;
					case 0xAD: segment += "\u30B9"; break;
					case 0xAE: segment += "\u30BB"; break;
					case 0xAF: segment += "\u30BD"; break;
					case 0xB0: segment += "\u30BF"; break;
					case 0xB1: segment += "\u30C1"; break;
					case 0xB2: segment += "\u30C4"; break;
					case 0xB3: segment += "\u30C6"; break;
					case 0xB4: segment += "\u30C8"; break;
					case 0xB5: segment += "\u30CA"; break;
					case 0xB6: segment += "\u30CB"; break;
					case 0xB7: segment += "\u30CC"; break;
					case 0xB8: segment += "\u30CD"; break;
					case 0xB9: segment += "\u30CE"; break;
					case 0xBA: segment += "\u30CF"; break;
					case 0xBB: segment += "\u30D2"; break;
					case 0xBC: segment += "\u30D5"; break;
					case 0xBD: segment += "\u30D8"; break;
					case 0xBE: segment += "\u30DB"; break;
					case 0xBF: segment += "\u30DE"; break;
					case 0xC0: segment += "\u30DF"; break;
					case 0xC1: segment += "\u30E0"; break;
					case 0xC2: segment += "\u30E1"; break;
					case 0xC3: segment += "\u30E2"; break;
					case 0xC4: segment += "\u30E4"; break;
					case 0xC5: segment += "\u30E6"; break;
					case 0xC6: segment += "\u30E8"; break;
					case 0xC7: segment += "\u30E9"; break;
					case 0xC8: segment += "\u30EA"; break;
					case 0xC9: segment += "\u30EB"; break;
					case 0xCA: segment += "\u30EC"; break;
					case 0xCB: segment += "\u30ED"; break;
					case 0xCC: segment += "\u30EF"; break;
					case 0xCD: segment += "\u30F2"; break;
					case 0xCE: segment += "\u30E3"; break;
					case 0xCF: segment += "\u30E5"; break;
					case 0xD0: segment += "\u30E7"; break;
					case 0xD1: segment += "\u30A1"; break;
					case 0xD2: segment += "\u30A3"; break;
					case 0xD3: segment += "\u30C3"; break;
					case 0xD4: segment += "\u30F3"; break;
					case 0xD5: segment += "\u30A5"; break;
					case 0xD6: segment += "\u30A7"; break;
					case 0xD7: segment += "\u30A9"; break;
					case 0xD8: segment += "\u309B"; break;
					case 0xD9: segment += "\u309C"; break;
				}
			}

			result += segment;
		}
	}

	return result;
}

/****** Reads a list of IP addresses and ports that correspond with a given phone number ******/
bool AGB_MMU::campho_read_contact_list()
{
	campho.phone_number_to_ip_addr.clear();
	campho.phone_number_to_port.clear();

	std::string filename = config::data_path + "campho_contacts.txt";

	std::string input_line = "";
	std::ifstream file(filename.c_str(), std::ios::in);

	if(!file.is_open())
	{
		std::cout<<"MMU::Error - Could not open Campho contact data from " << filename << "\n";
		return false;
	}

	//Parse line for filename, time, and any other data. Data is separated by a colon
	while(getline(file, input_line))
	{
		if(!input_line.empty())
		{
			std::size_t parse_symbol;
			s32 pos = 0;
			s32 end_pos;

			std::string phone_number = "";
			std::string ip_address = "";
			std::string port_number = "";

			u32 progress = 0;

			//Grab phone number
			parse_symbol = input_line.find(":", pos);

			if(parse_symbol != std::string::npos)
			{
				phone_number = input_line.substr(pos, parse_symbol);
				pos += parse_symbol;
				progress++;
			}

			//Grab phone number
			parse_symbol = input_line.find(":", pos);

			if(parse_symbol != std::string::npos)
			{
				ip_address = input_line.substr((pos + 1), parse_symbol);
				pos += parse_symbol;
				progress++;
			}

			//Grab port number
			if((pos + 2) <= input_line.size())
			{
				port_number = input_line.substr(pos + 2);
				progress++;
			}

			if(progress == 3)
			{
				campho.phone_number_to_ip_addr[phone_number] = ip_address;
				campho.phone_number_to_port[phone_number] = port_number;
			}
		}
	} 

	file.close();
	return true;
}

/****** Processes network communications for the Campho Advance ******/
void AGB_MMU::campho_process_networking()
{
	if(!campho.network_init) { return; }

	#ifdef GBE_NETPLAY

	if(campho.net_buffer.size() < 0x10000) { campho.net_buffer.resize(0x10000); }

	switch(campho.network_state)
	{
		//Listen for incoming phone calls
		case 0x00:
			if(!campho.ringer.connected)
			{
				if(campho.ringer.remote_socket = SDLNet_TCP_Accept(campho.ringer.host_socket))
				{
					std::cout<<"MMU::Incoming Campho Call Detected\n";
					SDLNet_TCP_AddSocket(campho.phone_sockets, campho.ringer.host_socket);
					SDLNet_TCP_AddSocket(campho.phone_sockets, campho.ringer.remote_socket);
					campho.ringer.connected = true;
					campho.ringer.remote_init = true;
					campho.is_call_incoming = true;
					campho.network_state = 1;
				}
			}

			break;

		//Listen for port to communicate with caller
		case 0x01:
			//Check the status of connection
			SDLNet_CheckSockets(campho.phone_sockets, 0);

			//If this socket is active, receive the transfer
			if(SDLNet_SocketReady(campho.ringer.remote_socket))
			{
				//Grab 16-bit value from caller, i.e. the port this Campho Advance will use for the call
				//Data should be LSB first
				if(SDLNet_TCP_Recv(campho.ringer.remote_socket, campho.net_buffer.data(), 2) > 0)
				{
					campho.phone_out_port = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
					std::cout<<"MMU::Campho Caller TCP Port: " << std::dec << campho.phone_out_port << std::hex << "\n";

					//Get IP address of Campho calling us
					IPaddress* remote_ip = SDLNet_TCP_GetPeerAddress(campho.ringer.remote_socket);

					if(SDLNet_ResolveHost(&campho.line.host_ip, util::ip_to_str(remote_ip->host).c_str(), campho.phone_out_port) < 0)
					{
						std::cout<<"MMU::Error - Campho Phone Line could not resolve hostname\n";
						campho.network_state = 0;
						return;
					}

					if(!(campho.line.host_socket = SDLNet_TCP_Open(&campho.line.host_ip)))
					{
						std::cout<<"MMU::Error - Campho Phone could not open a connection on Port " << campho.phone_in_port << "\n";
						campho.network_state = 0;
						return;
					}

					campho.line.host_init = (campho.line.host_socket == NULL) ? false : true;
					campho.network_state = 2;
				}
			}

			break;

		//Send local input port to remote Campho Advance (via line)
		case 0x02:
			campho.net_buffer[0] = campho.phone_in_port;
			campho.net_buffer[1] = (campho.phone_in_port >> 8);
			SDLNet_TCP_Send(campho.line.host_socket, (void*)campho.net_buffer.data(), 2);
			campho.network_state = 3;
			break;

		//Check for disconnection (caller hangs up) before phone call is answered
		case 0x03:
			SDLNet_CheckSockets(campho.phone_sockets, 0);

			if(SDLNet_SocketReady(campho.ringer.remote_socket))
			{
				if(SDLNet_TCP_Recv(campho.ringer.remote_socket, campho.net_buffer.data(), 2) > 0)
				{
					u16 status = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
					
					if(status == 0)
					{
						std::cout<<"MMU::Call Ended Remotely\n";
						campho.call_state = 5;
					}
				}
			}

			break;

		//Answer Phone Call - Send 0xFFFF to caller (via line)
		case 0x04:
			campho.net_buffer[0] = 0xFF;
			campho.net_buffer[1] = 0xFF;
			SDLNet_TCP_Send(campho.line.host_socket, (void*)campho.net_buffer.data(), 2);
			campho.network_state = 5;
			campho.is_call_video_enabled = true;

			//Enable small video frame
			campho.capture_video = true;
			campho.is_large_frame = false;
			campho.update_local_camera = true;

			//Small video frame = 58x48, drawn 35 and 13 lines at a time
			campho.video_frame_size = 58 * 35;

			campho.video_capture_counter = 0;
			campho.new_frame = false;
			campho.video_frame_slice = 0;
			campho.last_slice = 0;

			//Turn on microphone if possible
			if(config::use_microphone && apu_stat->mic_init)
			{
				apu_stat->is_mic_on = true;
				SDL_PauseAudioDevice(apu_stat->mic_id, 0);
			}

			apu_stat->ext_audio.playing = true;
			apu_stat->ext_audio.volume = 63;

			break;

		//Transfer Audio/Video data (as the receiver)
		case 0x05:
			//Receive kill signal if necessary
			SDLNet_CheckSockets(campho.phone_sockets, 0);

			while(SDLNet_SocketReady(campho.ringer.remote_socket))
			{
				u32 recv_bytes = SDLNet_TCP_Recv(campho.ringer.remote_socket, campho.net_buffer.data(), 0x10000);
				u16 status = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
				u16 size = (campho.net_buffer[3] << 8) | campho.net_buffer[2];
				u32 len = (size & 0xFFFE) + 4;

				if(recv_bytes > 0)
				{
					switch(status)
					{
						case 0x0000:
							std::cout<<"MMU::Call Ended Remotely\n";
							campho.call_state = 5;

							//Turn off microphone if possible
							if(config::use_microphone && apu_stat->mic_init)
							{
								apu_stat->is_mic_on = false;
								SDL_PauseAudioDevice(apu_stat->mic_id, 1);
							}

							apu_stat->ext_audio.playing = false;
							apu_stat->ext_audio.volume = 0;

							return;

						case 0x1111:
							std::cout<<"VIDEO DATA RECV'D\n";

							campho.update_remote_camera = true;
							campho.remote_capture_buffer.clear();
			
							for(u32 x = 4; x < campho.net_buffer.size(); x++)
							{
								campho.remote_capture_buffer.push_back(campho.net_buffer[x]);
							}

							break;

						case 0x2222:
							std::cout<<"AUDIO DATA RECV'D\n";

							for(u32 x = 4; x < len; x += 2)
							{
								u8 lo = campho.net_buffer[x];
								u8 hi = campho.net_buffer[x + 1];

								u16 sample = (hi << 8) | lo;
								campho.microphone_in_buffer.push_back(sample);
							}

							break;
					}
				}
			}

			//Send audio/video data
			if(campho.send_video_data)
			{
				//Internal GBE+ header for video transfers. Type and Data Len. Actual data follows
				campho.net_buffer[0] = 0x11;
				campho.net_buffer[1] = 0x11;
				campho.net_buffer[2] = 0x00;
				campho.net_buffer[3] = 0x63;

				//Use webcam data if available
				if(!campho.web_cam_img.empty())
				{
					campho_get_image_data(campho.web_cam_img.data(), campho.net_buffer, 176, 144, true);
				} 
			
				//Use static BMP
				else
				{
					SDL_Surface* source = SDL_LoadBMP(config::external_camera_file.c_str());

					if(source != NULL)
					{
						u8* cam_pixel_data = (u8*)source->pixels;
						campho_get_image_data(cam_pixel_data, campho.net_buffer, source->w, source->h, true);	
					}

					SDL_FreeSurface(source);
				}

				SDLNet_TCP_Send(campho.line.host_socket, (void*)campho.net_buffer.data(), 0xC604);

				campho.send_video_data = false;
			}

			if(campho.send_audio_data)
			{
				u32 size = campho.microphone_out_buffer.size();
				if(size > 0xFFFB) { size = 0xFFFB; }

				//Internal GBE+ header for video transfers. Type and Data Len. Actual data follows
				campho.net_buffer[0] = 0x22;
				campho.net_buffer[1] = 0x22;
				campho.net_buffer[2] = (size & 0xFF);
				campho.net_buffer[3] = (size >> 8);

				for(u32 x = 0; x < size; x++) { campho.net_buffer[4 + x] = campho.microphone_out_buffer[x]; }

				SDLNet_TCP_Send(campho.line.host_socket, (void*)campho.net_buffer.data(), (size + 4));

				campho.microphone_out_buffer.clear();
				campho.send_audio_data = false;
			}

			break;

		//Setup connection to a remote Campho Advance as the caller
		case 0x80:
			{
				std::string ip_addr = "";
				u32 port = 0;

				//Look up IP addr and port based on called phone number
				if(campho.phone_number_to_ip_addr.find(campho.dialed_number) != campho.phone_number_to_ip_addr.end())
				{
					ip_addr = campho.phone_number_to_ip_addr[campho.dialed_number];
				}

				if(campho.phone_number_to_port.find(campho.dialed_number) != campho.phone_number_to_port.end())
				{
					util::from_str(campho.phone_number_to_port[campho.dialed_number], port);
					port &= 0xFFFF;
					campho.ringer.port = port;
				}

				//Abort whole process if something went wrong
				if(ip_addr.empty() || !port)
				{
					campho.network_state = 0;
					return;
				}

				//Setup client, listen on another port
				if(SDLNet_ResolveHost(&campho.ringer.remote_ip, ip_addr.c_str(), campho.ringer.port) < 0)
				{
					std::cout<<"MMU::Error - Could not resolve hostname for Campho Advance calling.\n";
					campho.network_state = 0;
					return;
				}

				campho.network_state = 0x81;

				std::cout<<"MMU::Campho Calling " << ip_addr << ":" << std::dec << port << std::hex << "... \n";
			}

			break;

		//Establish a connection to a remote Campho Advance as the caller
		case 0x81:
			if(campho.ringer.remote_socket = SDLNet_TCP_Open(&campho.ringer.remote_ip))
			{
				std::cout<<"MMU::Connected to Campho Advance\n";
				SDLNet_TCP_AddSocket(campho.phone_sockets, campho.ringer.remote_socket);
				campho.ringer.connected = true;
				campho.ringer.host_init = true;
				campho.network_state = 0x82;

				//Listen for any incoming connections from remote Campho
				if(SDLNet_ResolveHost(&campho.line.host_ip, NULL, campho.phone_in_port) < 0)
				{
					std::cout<<"MMU::Error - Campho Phone Line could not resolve hostname\n";
					campho.network_state = 0;
					return;
				}

				if(!(campho.line.host_socket = SDLNet_TCP_Open(&campho.line.host_ip)))
				{
					std::cout<<"MMU::Error - Campho Phone could not open a connection on Port " << campho.phone_in_port << "\n";
					campho.network_state = 0;
					return;
				}

				campho.line.host_init = (campho.line.host_socket == NULL) ? false : true;
			}	

			break;

		//Continually send local input port to remote Campho Advance (via ringer)
		//Wait for confirmation by checking input port for activity (via line)
		case 0x82:
			campho.net_buffer[0] = campho.phone_in_port;
			campho.net_buffer[1] = (campho.phone_in_port >> 8);
			SDLNet_TCP_Send(campho.ringer.remote_socket, (void*)campho.net_buffer.data(), 2);

			if(!campho.line.connected)
			{
				if(campho.line.remote_socket = SDLNet_TCP_Accept(campho.line.host_socket))
				{
					std::cout<<"MMU::Campho Advance Phone Line Established\n";
					SDLNet_TCP_AddSocket(campho.phone_sockets, campho.line.host_socket);
					SDLNet_TCP_AddSocket(campho.phone_sockets, campho.line.remote_socket);
					campho.line.connected = true;
					campho.line.remote_init = true;

					//Receive 16-bit port to communicate with remote Campho Advance
					//Technically this is blocking, but the TCP data should be available immediately at this point
					if(SDLNet_TCP_Recv(campho.line.remote_socket, campho.net_buffer.data(), 2) > 0)
					{
						campho.phone_out_port = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
						campho.network_state = 0x83;
						std::cout<<"MMU::Campho Receiver TCP Port: " << std::dec << campho.phone_out_port << std::hex << "\n";
					}
				}
			}

			break;

		//Wait for remote Campho Advance to answer phone call
		case 0x83:
			SDLNet_CheckSockets(campho.phone_sockets, 0);

			if(SDLNet_SocketReady(campho.line.remote_socket))
			{
				if(SDLNet_TCP_Recv(campho.line.remote_socket, campho.net_buffer.data(), 2) > 0)
				{
					if((campho.net_buffer[0] == 0xFF) && (campho.net_buffer[1] == 0xFF))
					{
						campho.network_state = 0x84;
						campho.is_call_active = true;
						campho.is_call_video_enabled = true;
						campho.call_state = 0x03;

						//Enable small video frame
						campho.capture_video = true;
						campho.is_large_frame = false;
						campho.update_local_camera = true;

						//Small video frame = 58x48, drawn 35 and 13 lines at a time
						campho.video_frame_size = 58 * 35;

						campho.video_capture_counter = 0;
						campho.new_frame = false;
						campho.video_frame_slice = 0;
						campho.last_slice = 0;

						//Turn on microphone if possible
						if(config::use_microphone && apu_stat->mic_init)
						{
							apu_stat->is_mic_on = true;
							SDL_PauseAudioDevice(apu_stat->mic_id, 0);
						}

						apu_stat->ext_audio.playing = true;
						apu_stat->ext_audio.volume = 63;

						std::cout<<"MMU::Remote Campho Answered Call\n";
					}
				}
			}

			break;

		//Transfer Audio/Video data (as the caller)
		case 0x84:
			//Receive kill signal if necessary
			SDLNet_CheckSockets(campho.phone_sockets, 0);

			if(SDLNet_SocketReady(campho.ringer.remote_socket))
			{
				if(SDLNet_TCP_Recv(campho.ringer.remote_socket, campho.net_buffer.data(), 2) > 0)
				{
					u16 status = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
					
					if(status == 0)
					{
						std::cout<<"MMU::Call Ended Remotely\n";
						campho.call_state = 5;
						return;
					}
				}
			}

			while(SDLNet_SocketReady(campho.line.remote_socket))
			{
				u32 recv_bytes = SDLNet_TCP_Recv(campho.line.remote_socket, campho.net_buffer.data(), 0x10000);
				u16 status = (campho.net_buffer[1] << 8) | campho.net_buffer[0];
				u16 size = (campho.net_buffer[3] << 8) | campho.net_buffer[2];
				u32 len = (size & 0xFFFE) + 4;
				
				if(recv_bytes > 0)
				{
					switch(status)
					{
						case 0x1111:
							std::cout<<"VIDEO DATA RECV'D\n";

							campho.update_remote_camera = true;
							campho.remote_capture_buffer.clear();
			
							for(u32 x = 4; x < campho.net_buffer.size(); x++)
							{
								campho.remote_capture_buffer.push_back(campho.net_buffer[x]);
							}

							break;

						case 0x2222:
							std::cout<<"AUDIO DATA RECV'D\n";

							for(u32 x = 4; x < len; x += 2)
							{
								u8 lo = campho.net_buffer[x];
								u8 hi = campho.net_buffer[x + 1];

								u16 sample = (hi << 8) | lo;
								campho.microphone_in_buffer.push_back(sample);
							}

							break;
					}
				}
			}

			//Send audio/video data
			if(campho.send_video_data)
			{
				//Internal GBE+ header for video transfers. Type and Data Len. Actual data follows
				campho.net_buffer[0] = 0x11;
				campho.net_buffer[1] = 0x11;
				campho.net_buffer[2] = 0x00;
				campho.net_buffer[3] = 0x63;

				//Use webcam data if available
				if(!campho.web_cam_img.empty())
				{
					campho_get_image_data(campho.web_cam_img.data(), campho.net_buffer, 176, 144, true);
				} 
			
				//Use static BMP
				else
				{
					SDL_Surface* source = SDL_LoadBMP(config::external_camera_file.c_str());

					if(source != NULL)
					{
						u8* cam_pixel_data = (u8*)source->pixels;
						campho_get_image_data(cam_pixel_data, campho.net_buffer, source->w, source->h, true);	
					}

					SDL_FreeSurface(source);
				}

				SDLNet_TCP_Send(campho.ringer.remote_socket, (void*)campho.net_buffer.data(), 0xC604);

				campho.send_video_data = false;
			}

			if(campho.send_audio_data)
			{
				u32 size = campho.microphone_out_buffer.size();
				if(size > 0xFFFB) { size = 0xFFFB; }

				//Internal GBE+ header for video transfers. Type and Data Len. Actual data follows
				campho.net_buffer[0] = 0x22;
				campho.net_buffer[1] = 0x22;
				campho.net_buffer[2] = (size & 0xFF);
				campho.net_buffer[3] = (size >> 8);

				for(u32 x = 0; x < size; x++) { campho.net_buffer[4 + x] = campho.microphone_out_buffer[x]; }

				SDLNet_TCP_Send(campho.ringer.remote_socket, (void*)campho.net_buffer.data(), (size + 4));

				campho.microphone_out_buffer.clear();
				campho.send_audio_data = false;
			}

			break;

		//Terminate networking (no restart)
		case 0xFE:
			if(campho.ringer.connected)
			{
				campho.net_buffer[0] = 0;
				campho.net_buffer[1] = 0;
				SDLNet_TCP_Send(campho.ringer.remote_socket, (void*)campho.net_buffer.data(), 2);
			}

			//Turn off microphone if possible
			if(config::use_microphone && apu_stat->mic_init)
			{
				apu_stat->is_mic_on = false;
				SDL_PauseAudioDevice(apu_stat->mic_id, 1);
			}

			apu_stat->ext_audio.playing = false;
			apu_stat->ext_audio.volume = 0;

			break;

		//Terminate networking and restart
		case 0xFF:
			if(campho.ringer.connected)
			{
				campho.net_buffer[0] = 0;
				campho.net_buffer[1] = 0;
				SDLNet_TCP_Send(campho.ringer.remote_socket, (void*)campho.net_buffer.data(), 2);
			}

			campho_close_network();
			campho_reset_network();

			//Turn off microphone if possible
			if(config::use_microphone && apu_stat->mic_init)
			{
				apu_stat->is_mic_on = false;
				SDL_PauseAudioDevice(apu_stat->mic_id, 1);
			}

			apu_stat->ext_audio.playing = false;
			apu_stat->ext_audio.volume = 0;

			break;
	}

	//HTTP-based Web UI is a separate interface for receiving webcam data via a networked app
	//Used for local camera input in place of a static BMP or native webcam support (whenever GBE+ switches to SDL3)
	//GBE+ includes a Javascript-based demo designed to work with this
	//This is *input-only*. Client sends webcam image, GBE+ sends HTTP response... and that's it!
	//Data is grabbed once per-frame, non-blocking, so TCP transfer may take a bit to finish

	//Start new HTTP POST request
	if(!campho.web.transfer_start)
	{
		if(campho.web.remote_socket = SDLNet_TCP_Accept(campho.web.host_socket))
		{
			campho.web.transfer_start = true;
			campho.web.recv_bytes = 0;
			campho.web.timeout = SDL_GetTicks();
			campho.web_cam_buffer.clear();

			SDLNet_TCP_AddSocket(campho.phone_sockets, campho.web.host_socket);
			SDLNet_TCP_AddSocket(campho.phone_sockets, campho.web.remote_socket);
		}
	}

	//Grab data for HTTP POST request
	if(campho.web.transfer_start)
	{
		u32 diff = campho.web.recv_bytes;
		u32 ticks = SDL_GetTicks() - campho.web.timeout;

		SDLNet_CheckSockets(campho.phone_sockets, 0);

		if(SDLNet_SocketReady(campho.web.remote_socket))
		{
			campho.web.recv_bytes += SDLNet_TCP_Recv(campho.web.remote_socket, campho.net_buffer.data(), 0x10000);
		}

		diff = campho.web.recv_bytes - diff;

		//Copy new data into webcam buffer
		for(u32 x = 0; x < diff; x++) { campho.web_cam_buffer.push_back(campho.net_buffer[x]); }

		//Finish HTTP POST with response
		//We are being lazy here. Just make sure ~76KB are sent, which generally means we got all of it
		//Timeout the connection early on this side. Technically report 200 status, but disregard the data
		if((campho.web.recv_bytes >= 76000) || (ticks > 500))
		{
			campho.web.transfer_start = false;

			//Generate and send HTTP response
			std::string http_str = "HTTP/1.1 200 OK\r\n";
			http_str += "Access-Control-Allow-Origin: *\r\n\r\n";

			std::vector <u8> http_data;
			http_data.resize(http_str.length());
			util::str_to_data(http_data.data(), http_str);

			u32 send_bytes = SDLNet_TCP_Send(campho.web.remote_socket, (void*)http_data.data(), http_data.size());

			//Close connection to signal end of transfer
			SDLNet_TCP_DelSocket(campho.phone_sockets, campho.web.host_socket);
			SDLNet_TCP_DelSocket(campho.phone_sockets, campho.web.remote_socket);
			SDLNet_TCP_Close(campho.web.remote_socket);

			//Parse complete POST data for RGB image
			if(ticks <= 500)
			{
				std::string raw_str = util::data_to_str(campho.web_cam_buffer.data(), campho.web_cam_buffer.size());
				std::size_t header_match = raw_str.find("\r\n\r\n");

				if(header_match != std::string::npos)
				{
					raw_str = raw_str.substr(header_match + 4);
					campho.web_cam_img.resize(raw_str.length());
					util::str_to_data(campho.web_cam_img.data(), raw_str);
					campho.update_local_camera = true;
				}
			}
		}
	}

	#endif
}

/****** Cleans up any networking related to the Campho Advance ******/
void AGB_MMU::campho_close_network()
{
	if(!campho.network_init) { return; }

	#ifdef GBE_NETPLAY

	//Close SDL_net and any current connections
	if(campho.line.host_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.line.host_socket);
		SDLNet_TCP_Close(campho.line.host_socket);
	}

	if(campho.line.remote_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.line.remote_socket);
		SDLNet_TCP_Close(campho.line.remote_socket);
	}

	if(campho.ringer.host_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.ringer.host_socket);
		SDLNet_TCP_Close(campho.ringer.host_socket);
	}

	if(campho.ringer.remote_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.ringer.remote_socket);
		SDLNet_TCP_Close(campho.ringer.remote_socket);
	}

	if(campho.web.host_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.web.host_socket);
		SDLNet_TCP_Close(campho.web.host_socket);
	}

	if(campho.web.remote_socket != NULL)
	{
		SDLNet_TCP_DelSocket(campho.phone_sockets, campho.web.remote_socket);
		SDLNet_TCP_Close(campho.web.remote_socket);
	}

	campho.line.connected = false;
	campho.line.host_init = false;
	campho.line.remote_init = false;

	campho.ringer.connected = false;
	campho.ringer.host_init = false;
	campho.ringer.remote_init = false;

	campho.web.connected = false;
	campho.web.host_init = false;
	campho.web.remote_init = false;

	campho.network_init = false;
	campho.phone_out_port = 0;

	SDLNet_Quit();

	#endif
}

/****** Resets any networking related to the Campho Advance ******/
void AGB_MMU::campho_reset_network()
{
	#ifdef GBE_NETPLAY

	if(SDLNet_Init() >= 0)
	{
		campho.network_init = true;
		campho.phone_in_port = config::campho_input_port;

		campho.line.host_socket = NULL;
		campho.line.host_init = false;
		campho.line.remote_socket = NULL;
		campho.line.remote_init = false;
		campho.line.connected = false;

		campho.ringer.host_socket = NULL;
		campho.ringer.host_init = false;
		campho.ringer.remote_socket = NULL;
		campho.ringer.remote_init = false;
		campho.ringer.connected = false;
		campho.ringer.port = config::campho_ringer_port;

		campho.web.host_socket = NULL;
		campho.web.host_init = false;
		campho.web.remote_socket = NULL;
		campho.web.remote_init = false;
		campho.web.connected = false;
		campho.web.port = config::campho_web_port;
		campho.web.transfer_start = false;
		campho.web.recv_bytes = 0;

		campho.net_buffer.clear();
		campho.net_buffer.resize(0x8000, 0);
		campho.network_state = 0;
		campho.is_call_incoming = false;
		campho.is_call_active = false;
		campho.is_call_video_enabled = false;
		campho.send_video_data = false;
		campho.send_audio_data = false;

		//Setup ringer to listen for any incoming connections
		if(SDLNet_ResolveHost(&campho.ringer.host_ip, NULL, campho.ringer.port) < 0)
		{
			std::cout<<"MMU::Error - Campho Ringer could not resolve hostname\n";
			campho.network_init = false;
			return;
		}

		if(!(campho.ringer.host_socket = SDLNet_TCP_Open(&campho.ringer.host_ip)))
		{
			std::cout<<"SIO::Error - Campho Ringer could not open a connection on Port " << campho.ringer.port << "\n";
			campho.network_init = false;
			return;
		}

		//Setup Web UI to listen for any incoming connections
		if(SDLNet_ResolveHost(&campho.web.host_ip, NULL, campho.web.port) < 0)
		{
			std::cout<<"MMU::Error - Campho Web UI could not resolve hostname\n";
			campho.network_init = false;
			return;
		}

		if(!(campho.web.host_socket = SDLNet_TCP_Open(&campho.web.host_ip)))
		{
			std::cout<<"SIO::Error - Campho Web UI could not open a connection on Port " << campho.web.port << "\n";
			campho.network_init = false;
			return;
		}

		campho.ringer.host_init = (campho.ringer.host_socket == NULL) ? false : true;
		campho.web.host_init = (campho.web.host_socket == NULL) ? false : true;

		//Create sockets sets
		campho.phone_sockets = SDLNet_AllocSocketSet(6);
	}

	#endif
}
