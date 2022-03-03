#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include <signal.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

#define MPEGTS_PACKET_SIZE 188
#define MPEGTS_HEADER_SIZE 4

#define VIDEO_FPS 25
#define VIDEO_GOP 25

// From FFMPEG
#define INITIAL_PCR 63000
#define DEFAULT_PES_ADTS_STREAM_ID 0xc0
#define DEFAULT_PES_H264_STREAM_ID 0x1b

#define DEFAULT_TS_FILE_DURATION 4000 // ms
#define VIDEO_FRAME_CLOCK 90000 / VIDEO_FPS // 33ms (90khz -> 1s)
#define AUDIO_FRAME_CLOCK 1920 // 90000 / 46.875
#define PES_H264_PID 256
#define PES_ADTS_PID 257
#define PES_H264_HEADER_SIZE 19
#define PES_ADTS_HEADER_SIZE 14

#define DEFAULT_PAT_INTERVAL 40 // interval in number of packets
#define DEFAULT_PMT_INTERVAL 40

#define H264_BUFFER_SIZE 32 * 1024 * 1024
#define ADTS_BUFFER_SIZE 32 * 1024 * 1024
#define ADTS_SAMPLES_PER_FRAME 1024
#define ADTS_SAMPLES_PER_SECOND 48000

#define OUTPUT_SEGMENT_PREFIX "mux"
#define HLS_PLAYLIST_FILENAME "playlist.m3u8"

typedef unsigned char u_char;
typedef enum { PMT, PAT, PES_ADTS, PES_H264, TS_UNKNOWN } ts_packet_type;
typedef enum { VCL, NON_VCL, IDR, SPS, PPS, AUD } nalu_type;

typedef struct {
	FILE* fileptr;
	u_char* frame;
	u_char* bufferptr;
	u_char* initbufferptr;
	int buffer_load_count;
	long initial_buffer_size;
	long loaded_buffer_size;
	long file_size;

	int frames_read;
	int pes_pid;
	int pes_cc;
	int continuity_counter;

	long frame_size_bytes;
	long initial_frame_size_bytes;

	unsigned long pcr;
	unsigned long pts;
	unsigned long dts;

	bool pes_initialized;
} output_stream;

typedef struct {
	FILE* segptr;
	FILE* hlsptr;

	int segment_index;

	unsigned curr_packet_idx;
	ts_packet_type curr_packet_type;
	int last_pat_idx;
	int last_pmt_idx;
	unsigned pat_cc;
	unsigned pmt_cc;
	
	unsigned long bytes_written;

	output_stream* audio_stream;
	output_stream* video_stream;
} ts_writer;

int find_adts_header(u_char* buf, int size, int* frame_start, int* frame_end) {
	/*
		ADTS header will have 7 bytes when the protection absent field is 1

		Look for 0xFFF1 (0xFFF -> syncword, 1 -> MPEG version = MPEG-4)
	*/
	int i = 0;
	*frame_start = *frame_end = 0;
	
	while (buf[i] != 0xff || buf[i + 1] != 0xf1) {
		i += 1;
		if (i + 1 > size) { return 0; }
	}

	*frame_start = i;

	i += 1;

	while (buf[i] != 0xff || buf[i + 1] != 0xf1) {
		i += 1;
		if (i + 1 > size) { return -1; }
	}
	*frame_end = i;

	return (*frame_end - *frame_start);
}

int find_nal_unit(u_char* buf, int size, int* nal_start, int* nal_end) {
	int i = 0;
	// find start
	*nal_start = 0;
	*nal_end = 0;

	while (   // look for 24 or 32-bit NALU start code
		(buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0x01) &&
		(buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0 || buf[i + 3] != 0x01)
		)
	{
		i += 1; // skip leading zero
		if (i + 4 >= size) { return 0; } // did not find nal start
	}

	if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0x01) { // ( next_bits( 24 ) != 0x000001 )
		i++;
	}

	if (buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0x01) { /* error, should never happen */ return 0; }
	i += 3;
	*nal_start = i;

	while (   //( next_bits( 24 ) != 0x000000 && next_bits( 24 ) != 0x000001 )
		(buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0) &&
		(buf[i] != 0 || buf[i + 1] != 0 || buf[i + 2] != 0x01)
		)
	{
		i++;
		// FIXME the next line fails when reading a nal that ends exactly at the end of the data
		if (i + 3 >= size) { *nal_end = size; return -1; } // did not find nal end, stream ended first
	}

	*nal_end = i;
	return (*nal_end - *nal_start);
}

/*
	VCL = Video Coding Layer

	VCL NALU contains picture data
	non-VCL NALU contains parameter sets
*/
nalu_type get_nalu_type(u_char* frame) {
	int nal_unit_type;
	int start = 0;
	u_char unit_type;

	if (frame[1] == 0x00 && frame[2] == 0x00 && frame[3] == 0x01) { 
		unit_type = frame[4];
	} else if (frame[0] == 0x00 && frame[1] == 0x00 && frame[2] == 0x01) {
		unit_type = frame[3];
	} else {
		return false;
	}

	nal_unit_type = unit_type & 0x1f;

	// IDR is preceeded by SPS -> PPS (SPS -> PPS -> IDR)
	if (nal_unit_type == 7) { return SPS; }
	if (nal_unit_type == 8) { return PPS; }
	if (nal_unit_type == 1) { return VCL; } 
	if (nal_unit_type == 5) { return IDR; }
	if (nal_unit_type == 9) { return AUD; }

	return NON_VCL;
}

void load_buffer(output_stream* stream) {
	// Do not create a buffer bigger than the input file
	long bufsize = MIN(stream->pes_pid == PES_H264_PID ? H264_BUFFER_SIZE : ADTS_BUFFER_SIZE, stream->file_size);
	u_char* buf = (u_char*)malloc(bufsize);
	
	// Free previously allocated buffer
	if (stream->bufferptr != NULL) {
		free(stream->initbufferptr);
	}

	fread(buf, 1, bufsize, stream->fileptr);
	stream->initial_buffer_size = bufsize;
	stream->loaded_buffer_size = bufsize;
	stream->bufferptr = buf;
	stream->initbufferptr = buf;
	stream->buffer_load_count += 1;
	stream->file_size -= bufsize;
}

void extract_frame_from_buffer(output_stream* stream, int frame_start, int frame_end) {
	/*
		The method responsible for writing PES payloads must also free the allocated frame
		once it has been completely consumed
	*/
	int frame_size = frame_end - frame_start;
	stream->frame = (u_char*)malloc(frame_size);
	stream->initial_frame_size_bytes = frame_size;
	stream->frame_size_bytes = stream->initial_frame_size_bytes;

	// Copy data from buffer to frame
	if (frame_start == frame_end && stream->file_size == 0) {
		memcpy(stream->frame, stream->bufferptr + frame_start, stream->loaded_buffer_size);
		stream->loaded_buffer_size = 0;
		stream->bufferptr = NULL;
	} else {
		memcpy(stream->frame, stream->bufferptr + frame_start, frame_end - frame_start);
		stream->loaded_buffer_size -= frame_end;
		stream->bufferptr += frame_end;
	}

	if (stream->pes_pid == PES_H264_PID) {
		if (get_nalu_type(stream->frame) == VCL || get_nalu_type(stream->frame) == SPS) {
			stream->frames_read += 1;
			stream->pts += VIDEO_FRAME_CLOCK;
			stream->pcr += VIDEO_FRAME_CLOCK;
		} 
	} else if (stream->pes_pid == PES_ADTS_PID) {
		// Need to get samples per frame from ADTS header
		int audio_frames = (stream->frame[6] & 0x03) + 1;
		stream->pcr += AUDIO_FRAME_CLOCK;
		stream->pts += AUDIO_FRAME_CLOCK * audio_frames;
#if _DEBUG
		printf("audio frame: %d || pts: %ld  || frame length %d || real fl: %d\n", 
			total_audio_frames, 
			stream->pts,
			((stream->frame[3] & 0x03) << 11) | ((stream->frame[4] & 0xff) << 3) | ((stream->frame[5] & 0xe0) >> 5), // skip 24 bits + mask first 6
			frame_end - frame_start
		);
#endif
	}
}

void load_frame(ts_writer* writer) {
	int frame_start, frame_end, res;
	output_stream* stream = writer->curr_packet_type == PES_H264 ? writer->video_stream : writer->audio_stream;
	
	if (stream->frame != NULL) {
		return;
	}
	
	if (stream->loaded_buffer_size == 0) {
		load_buffer(stream);
	}

	if (stream->pes_pid == PES_H264_PID) {
		while (find_nal_unit(stream->bufferptr, stream->loaded_buffer_size, &frame_start, &frame_end) < 1) {
			if (stream->file_size == 0) {
				break;
			}
			load_buffer(stream);
		}
		
		if (stream->bufferptr[frame_start - 4] != 0x00) {
			frame_start -= 3;
		} else {
			frame_start -= 4;
		}
		extract_frame_from_buffer(stream, frame_start, frame_end);
	} else if (stream->pes_pid == PES_ADTS_PID) {
		while (find_adts_header(stream->bufferptr, stream->loaded_buffer_size, &frame_start, &frame_end) < 1) {
			if (stream->file_size == 0) {
				break;
			}
			load_buffer(stream);
		}
		extract_frame_from_buffer(stream, frame_start, frame_end);
	}
}

void write_stuffing_bytes(ts_writer *writer) {
	int stuffing_byte = 0xff;

	// stuffing bytes
	for (size_t i = 0; i < (MPEGTS_PACKET_SIZE - writer->bytes_written); i++) {
		fwrite(&stuffing_byte, 1, 1, writer->segptr);
	}
}

output_stream* get_current_stream(ts_writer* writer) {
	if (writer->curr_packet_type == PES_ADTS) {
		return writer->audio_stream;
	}

	if (writer->curr_packet_type == PES_H264) {
		return writer->video_stream;
	}

	return NULL;
}

void writer_increment_bytes_written(ts_writer* writer, int size) {
	writer->bytes_written += size;

	if (writer->bytes_written > MPEGTS_PACKET_SIZE) {
		printf("Error: ts packet overflow\n");
		raise(SIGTERM);
	}
}

void write_to_ts_file(u_char* source, ts_writer *writer, int source_size) {
	fwrite(source, 1, source_size, writer->segptr);
	writer_increment_bytes_written(writer, source_size);
}

/*
	Payload references the Program Map Table (PID 4096)
*/
void write_pat(ts_writer *writer) {
	u_char pat_header[4] = { 0x47, 0x40, 0x00, 0x10 }; // 4 bytes  010 0000000000000 00 01 0000
	u_char pat_data_bytes[13] = { 0x00, 0x00, 0xb0, 0x0d, 0x00, 0x01, 0xc1, 0x00, 0x00, 0x00, 0x01, 0xf0, 0x00 };
	u_char pat_crc_32[4] = { 0x2a, 0xb1, 0x04, 0xb2 };
	// Set continuity counter
	pat_header[3] |= 0x0f & writer->pat_cc;
	
	write_to_ts_file(&pat_header[0], writer, 4);
	write_to_ts_file(&pat_data_bytes[0], writer, 13);
	write_to_ts_file(&pat_crc_32[0], writer, 4);
	
	writer->last_pat_idx = writer->curr_packet_idx;
	writer->pat_cc = (writer->pat_cc + 1) % 16;
}

/*
	Payload describes the stream types

	PID 0100 (256) -> Stream type 1b H.264/14496-10 video (MPEG-4/AVC)
    PID 0101 (257) -> Stream type 0f 13818-7 Audio with ADTS transport syntax
*/
void write_pmt(ts_writer *writer) {
	u_char pmt_header[4] = { 0x47, 0x50, 0x00, 0x10 };
	// Specifies a program map table with an H264 and an ADTS stream
	u_char pmt_data_bytes[29] = { 0x00, 0x02, 0xb0, 0x1d, 0x00, 0x01, 0xc1, 0x00, 0x00, 0xe1, 0x00, 0xf0, 0x00, 0x1b, 0xe1, 0x00, 0xf0, 0x00, 0x0f, 0xe1, 0x01, 0xf0, 0x06, 0x0a, 0x04, 0x75, 0x6e, 0x64, 0x00};
	u_char pmt_crc_32[4] = { 0x08, 0x7d, 0xe8, 0x77 };
	// Set continuity counter
	pmt_header[3] |= 0x0f & writer->pmt_cc;

	write_to_ts_file(&pmt_header[0], writer, 4);
	write_to_ts_file(&pmt_data_bytes[0], writer, 29);
	write_to_ts_file(&pmt_crc_32[0], writer, 4);

	writer->last_pmt_idx = writer->curr_packet_idx;
	// continuity counter is a 4 bit field that must be reseted on overflow
	writer->pmt_cc = (writer->pmt_cc + 1) % 16;
}

bool packet_has_pcr(ts_writer* writer) {
	output_stream* stream = get_current_stream(writer);

	return stream->pes_pid == PES_H264_PID && get_nalu_type(stream->frame) == SPS;
}

/*
	A PES packet will have an adaptation field if:
		- it has an I-frame, so we need to include the PCR
		- it is the beginning of an ADTS block
		- the remaining frame size is not enough to fill the ts packet, so we need to
		  insert stuffing bytes on the adaptation field section
*/
int adaptation_field_length(ts_writer* writer) {
	bool is_pes = writer->curr_packet_type == PES_H264 || writer->curr_packet_type == PES_ADTS;

	if (!is_pes) { return 0; }

	output_stream* stream = get_current_stream(writer);
	long pcr = stream->pcr;
	int pes_header_size = stream->pes_pid == PES_H264_PID ? PES_H264_HEADER_SIZE : PES_ADTS_HEADER_SIZE;
	int afsize = 0;

	if (packet_has_pcr(writer)) {
		afsize += 8; // 6 bytes from PCR + 2 bytes from AF fields
	}

	// Determine if we'll need to add stuffing bytes
	int curr_pkt_size = MPEGTS_HEADER_SIZE + afsize;

	if (packet_has_pcr(writer) || !stream->pes_initialized) {
		curr_pkt_size += pes_header_size;
	}

	if (stream->pes_pid == PES_H264_PID && get_nalu_type(stream->frame) == VCL) {
		curr_pkt_size += 6;
	}

	if (MPEGTS_PACKET_SIZE > (curr_pkt_size + stream->frame_size_bytes)) {
		afsize += (MPEGTS_PACKET_SIZE - stream->frame_size_bytes - curr_pkt_size);
	}

	return afsize;
}

void write_adaptation_field_section(ts_writer* writer) {
	/*															 	bits
		adaptation_field_length										8
		if (adaptation_field_length > 0) {
			discontinuity_indicator 								1
			random_access_indicator 								1
			elementary_stream_priority_indicator					1
			PCR_flag 												1
			OPCR_flag 												1
			splicing_point_flag 									1
			transport_private_data_flag 							1
			adaptation_field_extension_flag 						1
		}
		if (PCR_flag == '1') {
			program_clock_reference_base 							33
			reserved 												6
			program_clock_reference_extension 						9
		}
		for (i = 0; i < N; i++) {
			stuffing_byte 											8
		}
	*/
	int adapfield_size = adaptation_field_length(writer);

	if (adapfield_size == 0) { return; }

	long pcr = get_current_stream(writer)->pcr;
	int bytes_written = 0;

	u_char* field = (u_char*)malloc(adapfield_size);

	field[0] = (adapfield_size - 0x01);

	if (adapfield_size == 1) {
		write_to_ts_file(field, writer, adapfield_size);
		return;
	}

	field[1] = 0x00;
	bytes_written += 1;

	if (packet_has_pcr(writer)) {
		field[1] |= 0x50; // Set PCR flag and random access indicator to 1 on I-frames
		// Write PCR
		field[2] = (pcr >> 25);
		field[3] = 0xff & (pcr >> 17);
		field[4] = 0xff & (pcr >> 9);
		field[5] = 0xff & (pcr >> 1);
		field[6] = (0x01 & pcr) << 7;

		// Write reserved and extension field
		field[6] |= 0x7e;
		field[7] = 0x00;
		bytes_written += 6;
	}

	for (unsigned i = 1; i < MAX(adapfield_size - bytes_written, 0); i++) {
		// stuffing bytes
		field[bytes_written + i] = 0xff;
	}

	write_to_ts_file(field, writer, adapfield_size);
}

void write_pes_header(ts_writer* writer, int adapfield_size) {
	/*
	index													bits
	[0][1][2]		packet_start_code_prefix 				24
	[3]				stream_id 								8
	[4][5]			PES_packet_length 						16
	[6]				'10' 									2
	[6]				PES_scrambling_control 					2
	[6]				PES_priority 							1
	[6]				data_alignment_indicator 				1
	[6]				copyright 								1
	[6]				original_or_copy 						1
	[7]				PTS_DTS_flags 							2
	[7]				ESCR_flag 								1
	[7]				ES_rate_flag 							1
	[7]				DSM_trick_mode_flag 					1
	[7]				additional_copy_info_flag 				1
	[7]				PES_CRC_flag 							1
	[7]				PES_extension_flag 						1
	[8]				PES_header_data_length 					8
					
					if (PTS_DTS_flags == '11') {
						'0010' 								4
						PTS [32..30] 						3
						marker_bit 							1
						PTS [29..15] 						15
						marker_bit 							1
						PTS [14..0] 						15
						marker_bit 							1
						'0001' 								4
						DTS [32..30] 						3
						marker_bit 							1
						DTS [29..15] 						15
						marker_bit 							1
						DTS [14..0] 						15
						marker_bit							1
					}
	*/
	int pes_header_size = writer->curr_packet_type == PES_H264 ? PES_H264_HEADER_SIZE : PES_ADTS_HEADER_SIZE;
	u_char* pes_header = (u_char*)malloc(pes_header_size);
	long pts;

	// packet start code prefix (must have 24 bits, last bit = 1)
	pes_header[0] = 0x00;
	pes_header[1] = 0x00;
	pes_header[2] = 0x01;
	pes_header[8] = 0x05; // Size of optional PES fields (5 bytes only when PTS is present)
	
	if (writer->curr_packet_type == PES_ADTS) {
		pes_header[3] = DEFAULT_PES_ADTS_STREAM_ID;
		// PES packet length: cannot be 0 in audio elementary streams
		int aac_size = writer->audio_stream->initial_frame_size_bytes + 8;
		pes_header[4] = aac_size >> 8;
		pes_header[5] = 0xff & aac_size;
		pts = writer->audio_stream->pts;
		pes_header[7] = 0x80; // Setting PTS_DTS flag to 10 (PTS only)
		pes_header[9] = 0x20 | (0x07 & pts >> 30) | 0x01;
	} else {
		pes_header[3] = DEFAULT_PES_H264_STREAM_ID;
		pes_header[4] = 0x00;
		pes_header[5] = 0x00;
		pes_header[7] = 0xc0;
		pes_header[8] = 0x0a;

		pts = writer->video_stream->pts;

		// DTS (= PTS while we only support baseline)
		pes_header[9] = 0x30 | ((0x07 & pts >> 30) << 1) | 0x01;
		pes_header[14] = 0x10 | ((0x07 & pts >> 30) << 1) | 0x01;
		pes_header[15] = pts >> 22;
		pes_header[16] = ((pts >> 15) << 1) | 0x01;
		pes_header[17] = pts >> 7;
		pes_header[18] = pts | 0x01;
	}

	pes_header[6] = 0x80;

	// PTS
	pes_header[10] = pts >> 22;
	pes_header[11] = ((pts >> 15) << 1) | 0x01;
	pes_header[12] = pts >> 7;
	pes_header[13] = pts | 0x01;

	write_to_ts_file(&pes_header[0], writer, pes_header_size);
	
	free(pes_header);
}

unsigned write_pes_payload(ts_writer* writer) {
	u_char aud_nal_packet[6] = { 0x00, 0x00, 0x00, 0x01, 0x09, 0xf0 };
	u_char* payload_buffer;

	output_stream* stream = writer->curr_packet_type == PES_ADTS ? writer->audio_stream : writer->video_stream; 
	unsigned bytes_to_write = MIN(MPEGTS_PACKET_SIZE - writer->bytes_written, stream->frame_size_bytes);

	if (
		stream->pes_pid == PES_H264_PID && 
		(get_nalu_type(stream->frame) == VCL || get_nalu_type(stream->frame) == SPS) && 
		stream->frame_size_bytes == stream->initial_frame_size_bytes
	) {
		// add access unit delimiter
		write_to_ts_file(&aud_nal_packet[0], writer, 6);
		bytes_to_write = MIN(MPEGTS_PACKET_SIZE - writer->bytes_written, stream->frame_size_bytes);
	}
	write_to_ts_file(stream->frame, writer, bytes_to_write);
	stream->frame_size_bytes -= bytes_to_write;
	stream->frame += bytes_to_write;

	if (stream->frame_size_bytes <= 0) {
		if (stream->pes_pid == PES_H264_PID) {
			stream->frame = NULL;
			load_frame(writer);
			if (get_nalu_type(stream->frame) == VCL || get_nalu_type(stream->frame) == SPS) {
				stream->pes_initialized = false;
			}
		} else {
			stream->pes_initialized = false;
			stream->frame = NULL;
		}
	} 

	return bytes_to_write;
}

void add_segment_to_playlist(ts_writer *writer) {
	char segment_duration[32];
	char segment_filename[32];

	double vduration = (double)writer->video_stream->frames_read / VIDEO_FPS;
	
	sprintf(&segment_duration[0], "#EXTINF:%.3f\n", vduration);
	fputs(&segment_duration[0], writer->hlsptr);
	sprintf(segment_filename, "%s-%d.ts\n", OUTPUT_SEGMENT_PREFIX, writer->segment_index);
	fputs(segment_filename, writer->hlsptr);
	writer->segment_index += 1;
}

void init_next_ts_file(ts_writer* writer) {
	char segment_filename[32];

	add_segment_to_playlist(writer);
	fflush(writer->hlsptr);
	fclose(writer->segptr);
	sprintf(segment_filename, "%s-%d.ts", OUTPUT_SEGMENT_PREFIX, writer->segment_index);
	writer->segptr = fopen(segment_filename, "w");
	writer->audio_stream->frames_read = 0;
	writer->video_stream->frames_read = 0;
	writer->last_pat_idx = -DEFAULT_PAT_INTERVAL;
	writer->last_pmt_idx = -DEFAULT_PMT_INTERVAL;
}

unsigned write_pes_packet(ts_writer* writer) {
	// write ts packet header
	u_char ts_header[4] = { 0x47, 0x00, 0x00, 0x10 };
	output_stream* stream = get_current_stream(writer);
	
	load_frame(writer);

	ts_header[1] = 0x1f & (stream->pes_pid >> 8);
	ts_header[2] = 0xff & (stream->pes_pid);
	ts_header[3] |= (stream->pes_cc % 16);

	if (!stream->pes_initialized) {
		ts_header[1] |= 0x40; // set payload start field
	}

	if (adaptation_field_length(writer) > 0) {
		ts_header[3] |= 0x30; // adaptation_field_control = 3 (has adaptation field section and ts payload)
	}

	if (
		stream->pes_pid == PES_H264_PID &&
		(writer->video_stream->frames_read >= DEFAULT_TS_FILE_DURATION * VIDEO_FPS / 1000) &&
		get_nalu_type(stream->frame) == SPS
	) {
		init_next_ts_file(writer);
	}

	write_to_ts_file(&ts_header[0], writer, 4);
	write_adaptation_field_section(writer);

	if (!stream->pes_initialized) {
		write_pes_header(writer, adaptation_field_length(writer));
		stream->pes_initialized = true;
	}

	stream->pes_cc += 1;

	return write_pes_payload(writer);
}

void packet_type_to_write(ts_writer *writer) {
	bool vstream_emtpy = (writer->video_stream->file_size == 0 && writer->video_stream->loaded_buffer_size == 0);
	bool astream_emtpy = (writer->audio_stream->file_size == 0 && writer->audio_stream->loaded_buffer_size == 0);

	if (writer->curr_packet_idx - writer->last_pat_idx >= DEFAULT_PAT_INTERVAL)
		writer->curr_packet_type = PAT;
	else if (writer->curr_packet_idx - writer->last_pmt_idx >= DEFAULT_PMT_INTERVAL) {
		writer->curr_packet_type = PMT;
	} else if (!vstream_emtpy && writer->audio_stream->pts > writer->video_stream->pts && writer->audio_stream->frame_size_bytes == 0) {
		writer->curr_packet_type = PES_H264;
	} else if (!astream_emtpy) {
		writer->curr_packet_type = PES_ADTS;
	} else {
		writer->curr_packet_type = TS_UNKNOWN;
	}
}

void run_writer() {
	ts_packet_type packet_type;

	FILE* h264_input = fopen(getenv("TSMUX_H264_FILE"), "r");
	FILE* adts_input = fopen(getenv("TSMUX_ADTS_FILE"), "r");

	char segment_filename[16]; 
	int segment_index = 0;
	char hls_header[64];
	sprintf(segment_filename, "%s-%d.ts", OUTPUT_SEGMENT_PREFIX, segment_index);
	FILE* hls = fopen(HLS_PLAYLIST_FILENAME, "w");
	FILE* segment = fopen(segment_filename, "w");

	// Init HLS
	sprintf(hls_header, "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:%d\n", DEFAULT_TS_FILE_DURATION / 1000);
	fputs(hls_header, hls);

	// Get file sizes
	fseek(h264_input, 0, SEEK_END);
	output_stream vstream = {
		.fileptr = h264_input,
		.file_size = ftell(h264_input),
		.frame = NULL,
		.frame_size_bytes = 0,
		.initial_frame_size_bytes = 0,
		.pes_pid = PES_H264_PID,
		.continuity_counter = 0,
		.pcr = INITIAL_PCR,
		.pts = INITIAL_PCR * 2,
		.dts = INITIAL_PCR * 2,
		.pes_initialized = false
	};
	fseek(h264_input, 0, SEEK_SET);

	fseek(adts_input, 0, SEEK_END);
	output_stream astream = {
		.fileptr = adts_input,
		.file_size = ftell(adts_input),
		.frame = NULL,
		.frame_size_bytes = 0,
		.initial_frame_size_bytes = 0,
		.pes_pid = PES_ADTS_PID,
		.continuity_counter = 0,
		.pcr = INITIAL_PCR,
		.pts = INITIAL_PCR * 2,
		.dts = INITIAL_PCR * 2,
		.pes_initialized = false
	};
	fseek(adts_input, 0, SEEK_SET);

	ts_writer writer = {
		.segptr = segment,
		.hlsptr = hls,
		.last_pat_idx = -DEFAULT_PAT_INTERVAL,
		.last_pmt_idx = -DEFAULT_PMT_INTERVAL,
		.audio_stream = &astream,
		.video_stream = &vstream,
		.segment_index = 0
	};

	// Write one packet per iteration
	while (1) {
		packet_type_to_write(&writer);

		if (writer.curr_packet_type != TS_UNKNOWN) {
			writer.bytes_written = 0;
		}

		if (writer.curr_packet_type == PAT) {
			write_pat(&writer);
		} else if (writer.curr_packet_type == PMT) {
			write_pmt(&writer);
		} else if (writer.curr_packet_type == PES_ADTS) {
			write_pes_packet(&writer);
		} else if (writer.curr_packet_type == PES_H264) {
			write_pes_packet(&writer);
		} else {
			// Finished reading files, exit writer
			fclose(h264_input);
			fclose(adts_input);
			fclose(writer.segptr);
			add_segment_to_playlist(&writer);
			fputs("#EXT-X-ENDLIST", writer.hlsptr);
			fclose(writer.hlsptr);

			return;
		}

		write_stuffing_bytes(&writer);
		writer.curr_packet_idx += 1;
	}
}

int main() {
	run_writer();

	return 0;
}