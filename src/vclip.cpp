// extracts a clip of v210 video 
// by demuxing packets out of one MOV wrapper and muxing them directly back into another
// should preserve exact frames!

// followed several examples to put this together

// TODO: add better exception handling, now most exceptions just fail out opaquely

// allow windows builds which would otherwise
// compain about using fopen() instead of fopen_s()
// and sprintf() instead of sprintf_s(), etc...
#define _CRT_SECURE_NO_DEPRECATE

// CROPPING PARAMETERS FOR REDUICNG YUV422 (v210) VIDEO TO DA VINCI XI 720p FRAME SIZE
// NOTE: WILL LEAVE 1px BLACK BAR ON LEFT AND RIGHT, OTHERWISE WE'D BE SPLITING/SHIFTING CHROMA COMPONENTS
// ONCE FRAME IS CONVERTED TO RGB WE CAN CROP AGAIN
#define DAVINCI_CROP_WIDTH 896
#define DAVINCI_CROP_HEIGHT 714
#define DAVINCI_CROP_X 192       // zero indexed, first COLUMN to include in cropped ouput, MUST BE EVEN FOR YUV422
#define DAVINCI_CROP_Y 3         // zero indexed, first ROW to include in cropped output, MAY BE ODD FOR YUV422 (NOT FOR YUV420)

// need extern to include FFmpeg C libraries
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavutil/opt.h>
}
#include <iostream>
#include <string>
#include <cxxopts.hpp>  // https://www.github.com/jarro2783/cxxopts
#include <yaml-cpp/yaml.h>
#include <chrono>


// prepare a filter graph for cropping frames out of black border (da Vinci Xi)
// ref: https://stackoverflow.com/questions/38099422/how-to-crop-avframe-using-ffmpegs-functions
int crop_frame(const AVFrame* inframe, AVFrame* outframe, AVFilterContext* bufsrc_ctx, AVFilterContext* bufsnk_ctx) {

	// copy input frame to output frame
	if (av_frame_ref(outframe, inframe) < 0) {
		std::cout << "ERROR COPYING INPUT FRAME TO OUTPUT FRAME" << std::endl;
		return -1;
	}

	if (av_buffersrc_add_frame(bufsrc_ctx, outframe) < 0) {
		std::cout << "ERROR ADDING FRAME TO SOURCE BUFFER CONTEXT" << std::endl;
		return -1;
	}

	if (av_buffersink_get_frame(bufsnk_ctx, outframe) < 0) {
		std::cout << "ERROR ADDING FRAME TO SINK BUFFER CONTEXT" << std::endl;
		return -1;
	}

	// return 0 on success
	// cropped frame is in *outframe
	return 0;
}







int main(int argc, char** argv) {

	// INPUT-RELATED VARIABLES
	static AVFormatContext* ifmt_ctx = NULL; // input format context
	AVStream* istream = NULL;
	AVCodecParameters* codec_params = NULL;
	AVPacket* inpkt = NULL;

	// OUTPUT-RELATED VARIABLES
	static AVFormatContext* ofmt_ctx = NULL; // output format context
	AVStream* ostream = NULL;
	const AVCodec* enc = NULL;
	AVCodecContext* enc_ctx = NULL;
	AVCodecID outcodec_id = AV_CODEC_ID_NONE;

	// TRANSCODING-RELATED VARIABLES
	AVCodecContext* dec_ctx = NULL;

	// filter graph variables
	AVFilterContext* bufsrc_ctx = NULL;
	AVFilterContext* bufsnk_ctx = NULL;
	AVFilterGraph* fltgrph = avfilter_graph_alloc();
	AVFilterInOut* filt_in = avfilter_inout_alloc();
    AVFilterInOut* filt_out = avfilter_inout_alloc();
	AVFrame* frame_cropped = av_frame_alloc();
	char filtarg[255];

	// general variables
	static AVFrame* frame = NULL;
	YAML::Node config;
	unsigned int video_stream_idx = 0;
	bool found_video_stream = false;
	uint64_t my_frame_counter = 0;
	uint64_t firstframe, lastframe, num_frames_to_extract, pts_dts_scale;
	int prev_pct = -1;
	cxxopts::Options options("vcrop", "temporal video cropping");
	std::string infile_name, outfile_name, yamlfile_name;
	bool yaml_mode = false;
	bool compress_flag = false;
	bool framecrop_flag = false;
	bool transcode_flag = false;
	
	// struct and vector for storing clip details
	struct ClipDef {
		std::string name;
		uint64_t first_frame;
		uint64_t last_frame;
	};
	std::vector<ClipDef> clips;


	// PARSE COMMAND LINE OPTIONS
	try
	{
		options.add_options()
			("f,first", "number of first frame to include in output video", cxxopts::value<uint64_t>())
			("l,last", "number of last frame to inclue in output video", cxxopts::value<uint64_t>())
			("o,output", "name of output file", cxxopts::value<std::string>())
			("i,input", "name of input file", cxxopts::value<std::string>())
			("c", "flag to crop each frame to da Vinci Xi valid region", cxxopts::value<bool>()->default_value("false"))
			("z", "flag to compress output video", cxxopts::value<bool>()->default_value("false"))
			("y,yamlconfig", "name of config YAML file - use without setting any other options", cxxopts::value<std::string>());
		auto cxxopts_result = options.parse(argc, argv);

		if (cxxopts_result.count("yamlconfig") == 1)
		{
			yaml_mode = true;
			yamlfile_name = cxxopts_result["yamlconfig"].as<std::string>();
		}
		else if (
			(cxxopts_result.count("first") == 1) &&
			(cxxopts_result.count("last") == 1) &&
			(cxxopts_result.count("output") == 1) &&
			(cxxopts_result.count("input") == 1)) {
			yaml_mode = false;
			compress_flag = cxxopts_result["z"].as<bool>();
			framecrop_flag = cxxopts_result["c"].as<bool>();
			firstframe = cxxopts_result["first"].as<uint64_t>();
			lastframe = cxxopts_result["last"].as<uint64_t>();
			infile_name = cxxopts_result["input"].as<std::string>();
			outfile_name = cxxopts_result["output"].as<std::string>();

			// add our single clip to the clip storage vector
			struct ClipDef singleclip = { outfile_name, firstframe, lastframe };
			clips.push_back(singleclip);

		}
		else {
			std::cout << options.help() << std::endl;
			return -1;
		}
	}
	catch (const cxxopts::exceptions::exception& e)
	{
		std::cout << "Error parsing options: " << e.what() << std::endl;
		return -1;
	}



	// PARSE YAML CONFIG FILE
	// add each clip definition to our vector
	if (yaml_mode) {
		try {
			// get input file
			config = YAML::LoadFile(yamlfile_name);
			if (!config["input_file"]) {
				std::cout << "ERROR: No 'input_file' key found in YAML config file" << std::endl;
				return -1;
			}
			infile_name = config["input_file"].as<std::string>();
			//std::cout << "YAML input file: " << infile_name << std::endl;

			// get compression flag
			if (config["compress"]) {
				compress_flag = config["compress"].as<bool>();
				//std::cout << "YAML compression: " << compress_flag << std::endl;
			}
			else {
				compress_flag = false;
				std::cout << "YAML: no compression set, not compressing output" << std::endl;
			}

			// get frame crop flag
			if (config["framecrop"]) {
				framecrop_flag = config["framecrop"].as<bool>();
				//std::cout << "YAML framecrop: " << compress_flag << std::endl;
			}
			else {
				framecrop_flag = false;
				std::cout << "YAML: no framecrop set, not spatially cropping frames output" << std::endl;
			}

			// parse each clip definition
			if (!config["clips"]) {
				std::cout << "ERROR: no 'clips' key found in YAML config file" << std::endl;
				return -1;
			}
			YAML::Node yamlclips = config["clips"];
			for (YAML::const_iterator it = yamlclips.begin(); it != yamlclips.end(); ++it) {
				outfile_name = it->first.as<std::string>();
				YAML::Node clipdetails = it->second;
				firstframe = clipdetails["first"].as<uint64_t>();
				lastframe = clipdetails["last"].as<uint64_t>();
				std::cout << "YAML clip def: " << outfile_name << " [" << firstframe << "," << lastframe << "]" << std::endl;
				struct ClipDef singleclip = { outfile_name, firstframe, lastframe };
				clips.push_back(singleclip);
			}

			// make sure we have some clip definitions in our vector
			if (!clips.size()) {
				std::cout << "ERROR: no valid clip definitions found in YAML config file" << std::endl;
				return -1;
			}
		}
		catch (const YAML::Exception& e)
		{
			std::cout << "ERROR PARSING YAML CONFIGURATION: " << e.what() << std::endl;
			return -1;
		}
	}




	// INPUT FORMAT
	// open video file and read headers
	if (avformat_open_input(&ifmt_ctx, infile_name.c_str(), NULL, NULL) != 0) {
		std::cout << "Could not open file: " << infile_name << std::endl;
		return -1;
	}

	// get stream info from file
	if (avformat_find_stream_info(ifmt_ctx, NULL) < 0) {
		std::cout << "avformat_find_stream_info() failed!" << std::endl;
		return -1;
	}

	// print video format info to screen
	std::cout << std::endl;
	std::cout << "Input format: " << std::endl;
	av_dump_format(ifmt_ctx, 0, infile_name.c_str(), false);
	std::cout << std::endl;

	// look for a single v210-encoded video stream in this container
	for (unsigned int stream_idx = 0; stream_idx < ifmt_ctx->nb_streams; ++stream_idx) {
		istream = ifmt_ctx->streams[stream_idx];
		codec_params = istream->codecpar;
		printf("Located stream %02d: codec type = %03d, codec id = %03d\n",
			stream_idx,
			codec_params->codec_type,
			codec_params->codec_id);

		// determine whether this is v210, FFV1, or prores encoded video
		if (codec_params->codec_type == AVMEDIA_TYPE_VIDEO && (codec_params->codec_id == AV_CODEC_ID_V210 || codec_params->codec_id == AV_CODEC_ID_FFV1 || codec_params->codec_id == AV_CODEC_ID_PRORES)) {
			//std::cout << "FOUND v210, FFV1, OR PRORES ENCODED VIDEO" << std::endl;
			if (found_video_stream) {
				std::cout << "ERROR: MORE THAN ONE v210, FFV1, OR PRORES VIDEO STREAM FOUND!" << std::endl;
				return -1;
			}
			else {
				video_stream_idx = stream_idx;
				found_video_stream = true;
			}
		}
	}

	// error out if we didn't find v210 or FFV1 video
	if (!found_video_stream) {
		std::cout << "ERROR: DID NOT FIND A V210, FFV1, OR PRORES VIDEO STREAM" << std::endl;
		return -1;
	}

	// report success
	istream = ifmt_ctx->streams[video_stream_idx];
	codec_params = istream->codecpar;
	printf("Working with stream %02d: codec type = %03d, codec id = %03d\n",
		video_stream_idx,
		codec_params->codec_type,
		codec_params->codec_id);
	std::cout << "stream timebase: " << (istream->time_base).num << "/" << (istream->time_base).den << std::endl;


	// figure out transcoding, etc.
	// note: FFV1->FFV1 needs to be transcoded because we can't transmux packets starting from a non-keyframe
	//       PRORES->PRORES probably needs to be transcoded too...
	outcodec_id = compress_flag ? AV_CODEC_ID_FFV1 : codec_params->codec_id;
	if ((codec_params->codec_id == AV_CODEC_ID_V210) && (!compress_flag) && (!framecrop_flag)) {
		transcode_flag = false;
	}
	else {
		transcode_flag = true;
	}

	// set scaling
	pts_dts_scale = (uint64_t)av_q2d(av_mul_q(av_inv_q(istream->time_base), av_inv_q(istream->avg_frame_rate)));
	//std::cout << "pts_dts_scale = " << pts_dts_scale << std::endl;

	// allocate frame, this can happen anywhere
	// we will keep reusing the frame memory
	if ((frame = av_frame_alloc()) == 0) {
		std::cout << "ERROR: COULD NOT ALLOCATE FRAME POINTER" << std::endl;
		return -1;
	}


	// SET UP DECODER (only needed if transcoding - otherwise just transmux packets) 
	if (transcode_flag) {
		// open the decoder
		const AVCodec* dec = NULL;
		dec = avcodec_find_decoder(istream->codecpar->codec_id);
		if (!dec) {
			std::cout << "ERROR: COULD NOT FIND DECODER" << std::endl;
			return -1;
		}

		// allocate decoder context
		dec_ctx = avcodec_alloc_context3(dec);
		if (!dec_ctx) {
			std::cout << "ERROR: COULD NOT OPEN DECODER CONTEXT" << std::endl;
			return -1;
		}

		// estimate decoder framerate
		dec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, istream, NULL);

		// copy codec parameters from stream to decoder
		if (avcodec_parameters_to_context(dec_ctx, istream->codecpar) < 0) {
			std::cout << "ERROR: COULD NOT COPY CODEC PARAMS FROM STREAM TO DECODER" << std::endl;
			return -1;
		}

		// initialize decoder
		if (avcodec_open2(dec_ctx, dec, NULL) < 0) {
			std::cout << "ERROR: COULD NOT INITIALIZE DECODER" << std::endl;
			return -1;
		}
	}



	// PREPARE FILTER FOR CROPPING DOWN TO DAVINCI XI FRAME
	// prepare filter graph for cropping image to size
	// WE NEED THE DECODER ACTIVE TO DO THIS!
	// TODO: ADD A DIFFERENT FLAG (not just compress_flag)
	if (framecrop_flag && ((dec_ctx->width != 1280) || (dec_ctx->height != 720))) {
		framecrop_flag = false;
		//std::cout << "\033[1;35mInput video dimensions not compatible with cropping, output will not be cropped\033[0m" << std::endl;
		std::cout << "Input video dimensions not compatible with cropping, output will not be cropped" << std::endl;
	}
	if (framecrop_flag) {
		snprintf(filtarg, sizeof(filtarg),
			"buffer=video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=0/1[in];"
			"[in]crop=x=%d:y=%d:out_w=%d:out_h=%d[out];"
			"[out]buffersink",
			dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
			DAVINCI_CROP_X, DAVINCI_CROP_Y, DAVINCI_CROP_WIDTH, DAVINCI_CROP_HEIGHT);
		std::cout << "filtarg: " << filtarg << std::endl;
		if (avfilter_graph_parse2(fltgrph, filtarg, &filt_in, &filt_out) < 0) {
			std::cout << "ERROR PARSING FILTER GRAPH" << std::endl;
			return -1;
		}
		if (filt_in != NULL) {
			std::cout << "ERROR: NON-NULL FILTER INPUTS RETURNED BY avfilter_graph_parse2()" << std::endl;
			return -1;
		}
		if (filt_out != NULL) {
			std::cout << "ERROR: NON-NULL FILTER OUPUTS RETURNED BY avfilter_graph_parse2()" << std::endl;
			return -1;
		}
		if (avfilter_graph_config(fltgrph, NULL) < 0) {
			std::cout << "ERROR CONFIGURING FILTER GRAPH" << std::endl;
			return -1;
		}
		if ((bufsrc_ctx = avfilter_graph_get_filter(fltgrph, "Parsed_buffer_0")) == 0) {
			std::cout << "ERROR GETTING SOURCE FILTER CONTEXT" << std::endl;
			return -1;
		}
		if ((bufsnk_ctx = avfilter_graph_get_filter(fltgrph, "Parsed_buffersink_2")) == 0) {
			std::cout << "ERROR GETTING SINK FILTER CONTEXT" << std::endl;
			return -1;
		}
	}
		




	// ITERATE THROUGH ALL CLIP DEFINITIONS
	// EXTRACTING THE DESIRED SEGMENT OF VIDEO TO FILE
	//std::cout << "Analyzing " << infile_name << "..." << std::endl;
	for (auto& item : clips) {

		// reset frame counters
		my_frame_counter = 0;
		prev_pct = -1;

		// get clip definition parameters
		// NOTE: 'infile' is common and has already been set
		outfile_name = item.name;
		firstframe = item.first_frame;
		lastframe = item.last_frame;
		num_frames_to_extract = (lastframe - firstframe + 1);
		std::cout << "Processing: " << outfile_name << " [" << firstframe << "," << lastframe << "]" << std::endl;


		// SET UP OUTPUT FORMAT
		// NEED THIS HERE BECAUSE WE INITIAIZE OUTPUT FORMAT CONTEXT FOR EACH NEW FILE
		// allocate output format context
		if (avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, outfile_name.c_str()) < 0) {
			std::cout << "ERROR: COULD NOT CREATE OUTPUT CONTEXT" << std::endl;
			return -1;
		}

		// check to be sure we're muxing into an MOV container
		// TODO: might make this robust to other containers
		// although our intent 
		if (strcmp(ofmt_ctx->oformat->name, "mov")) {
			std::cout << "ERROR: CANNOT RELIABLY MUX INTO " << ofmt_ctx->oformat->name << std::endl;
			return -1;
		}
		else {
			std::cout << "Muxing into: " << ofmt_ctx->oformat->name << std::endl;
		}

		// single output stream for video
		if ((ostream = avformat_new_stream(ofmt_ctx, NULL)) == NULL) {
			std::cout << "ERROR: COULD NOT ALLOCATE OUTPUT STREAM" << std::endl;
			return -1;
		}

		// SET UP ENCODER (only needed if transcoding / compressing video - otherwise just transmux packets)
		if (transcode_flag) {
			
			if ((enc = avcodec_find_encoder(outcodec_id)) == 0) {
				std::cout << "ERROR: COULD NOT FIND ENCODER" << std::endl;
				return -1;
			}

			// allocate encoder context
			if ((enc_ctx = avcodec_alloc_context3(enc)) == 0) {
				std::cout << "ERROR: COULD NOT OPEN ENCODER CONTEXT" << std::endl;
				return -1;
			}

			// initialize encoder
			// bits per raw sample doesn't need to be set, presumably inferred from pix_fmt
			if (framecrop_flag) {
				enc_ctx->height = DAVINCI_CROP_HEIGHT;
				enc_ctx->width = DAVINCI_CROP_WIDTH;
			}
			else {
				enc_ctx->height = dec_ctx->height;
				enc_ctx->width = dec_ctx->width;
			}
			enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
			enc_ctx->pix_fmt = dec_ctx->pix_fmt;
			enc_ctx->time_base = dec_ctx->time_base;

			// gloabl header flag (following FFmpeg example)
			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
				enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

			if (avcodec_open2(enc_ctx, enc, NULL) < 0) {
				std::cout << "ERROR: COULD NOT INITIALIZE ENCODER" << std::endl;
				return -1;
			}
		}

		// update output stream and encoder codec parameters
		if (!transcode_flag) {
			// TRANSMUXING ONLY: get codec parameters from input stream
			if (avcodec_parameters_copy(ostream->codecpar, istream->codecpar) < 0) {
				std::cout << "ERROR: COULD NOT COPY CODEC PARAMETERS FROM INPUT STREAM TO OUTPUT STREAM" << std::endl;
				return -1;
			}
		}
		else {
			// TRANSCODING: get codec parametrs from encoder context
			if (avcodec_parameters_from_context(ostream->codecpar, enc_ctx) < 0) {
				std::cout << "ERROR: COULD NOT COPY CODEC PARAMETERS FROM ENCODER CONTEXT" << std::endl;
			}
		}

		// update output stream parameters
		ostream->sample_aspect_ratio.num = 1;
		ostream->sample_aspect_ratio.den = 1;
		ostream->time_base = istream->time_base;   // THIS IS KEY!
		ostream->avg_frame_rate = istream->avg_frame_rate;
	
		// open the output file
		if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
			std::cout << "Opening output file" << std::endl;
			if (avio_open(&ofmt_ctx->pb, outfile_name.c_str(), AVIO_FLAG_WRITE) < 0) {
				std::cout << "ERROR: COULD NOT OPEN OUTPUT FILE" << std::endl;
				return -1;
			}
		}

		// write header
		if (avformat_write_header(ofmt_ctx, NULL) < 0) {
			std::cout << "ERROR: COULD NOT WRITE OUTPUT FILE HEADER" << std::endl;
			return -1;
		}

		// check for a change of timebase
		// TODO: make this robust to different timebases?
		// not sure we want this, because we want the precision of 1/60000 for 59.94fps
		if (av_cmp_q(ostream->time_base, istream->time_base)) {
			std::wcout << "ERROR: avformat_write_header() CHANGED THE OUTPUT STREAM TIMEBASE TO " << ostream->time_base.num << "/" << ostream->time_base.den << std::endl;
			return -1;
		}

		// display output format
		std::cout << std::endl;
		std::cout << "Output format:" << std::endl;
		av_dump_format(ofmt_ctx, 0, outfile_name.c_str(), 1);
		std::cout << std::endl;

		// allocate packet pointer
		inpkt = av_packet_alloc();
		if (!inpkt) {
			std::cout << "ERROR: COULD NOT ALLOCATE DECODER PACKET POINTER" << std::endl;
			return -1;
		}

		// start timer
		auto start_time = std::chrono::high_resolution_clock::now();

		// find start of clip
		std::cout << "Seeking to first frame in clip" << std::endl;
		if (av_seek_frame(ifmt_ctx, video_stream_idx, firstframe * pts_dts_scale, AVSEEK_FLAG_BACKWARD) < 0){ // seeks to closest previous keyframe
			std::cout << "ERROR: SEEK FAILED" << std::endl;
			return -1;
		}

		// now we start reading
		bool found_frame = false;
		while ( (!found_frame) || (my_frame_counter < num_frames_to_extract)) {
			if (av_read_frame(ifmt_ctx, inpkt) < 0) {
				std::cout << "ERROR: COULD NOT READ FRAME" << std::endl;
				return -1;
			}
			if ((unsigned int)(inpkt->stream_index) == video_stream_idx) {

				// determine whether we're at our desired frame or if we've passed it
				// if we're before it we need to decode everything from the previous keyframe
				if (!found_frame) {
					if ((uint64_t)inpkt->dts == firstframe * pts_dts_scale) {    // we happened to seek right to where we wanted to be
						found_frame = true;
					}
					else if ((uint64_t)inpkt->dts > (firstframe * pts_dts_scale)) { // we missed our frame!
						std::cout << "ERROR: SEEK PASSED THE DESIRED FRAME!" << std::endl;
						return -1;
					}
				}

				// skip this packet if it is flagged to discard
				if (inpkt->flags & AV_PKT_FLAG_DISCARD) {
					std::cout << "Discarding packet." << std::endl;
					continue;
				}

				//std::cout << " Decoding frame " << (uint64_t)(inpkt->dts / pts_dts_scale) << std::endl;

				// correct PTS and DTS for output stream
				inpkt->pts = my_frame_counter * pts_dts_scale;
				inpkt->dts = my_frame_counter * pts_dts_scale;
				inpkt->time_base = istream->time_base;

				//std::cout << "Adding packet of size: " << inpkt->size << std::endl;
				//printf("PTS: %ld; DTS: %ld; POS: %ld; FLAG? %d\n",inpkt->pts,inpkt->dts,inpkt->pos, (inpkt->flags & AV_PKT_FLAG_KEY) ); 

				if (!transcode_flag) { // TRANSMUXING ONLY
					if (found_frame) {
						// mux packet into output format
						inpkt->stream_index = ostream->index;
						if (av_interleaved_write_frame(ofmt_ctx, inpkt) < 0) {
							std::cout << "ERROR: COULD NOT WRTIE PACKET TO OUTPUT STREAM" << std::endl;
							return -1;
						}
						++my_frame_counter;
					}
				}
				else { // TRANSCODING

					// send packet to decoder
					if (avcodec_send_packet(dec_ctx, inpkt) < 0) {
						std::cout << "ERROR SENDING PACKET TO DECODER" << std::endl;
						return -1;
					}

					// get all available frames from the decoder
					int retval = 0;
					while (retval >= 0) {
						if ((retval = avcodec_receive_frame(dec_ctx, frame)) == 0) {

							if (found_frame) {
								// crop frame if we want to do this
								if (framecrop_flag) {
									crop_frame(frame, frame_cropped, bufsrc_ctx, bufsnk_ctx);
									//std::cout << "Cropped frame dims: " << frame_cropped->width << "x" << frame_cropped->height << std::endl;
									av_frame_unref(frame);
									av_frame_ref(frame, frame_cropped);
								}


								// encode the decoded frame with the output codec
								AVPacket* outpkt = av_packet_alloc();
								int enc_resp = avcodec_send_frame(enc_ctx, frame);

								while (enc_resp >= 0) {
									enc_resp = avcodec_receive_packet(enc_ctx, outpkt);
									if (enc_resp == AVERROR(EAGAIN) || enc_resp == AVERROR_EOF) {
										//std::cout << "NO PACKET AVAILBLE FROM ENCODER" << std::endl;
										break;
									}
									else if (enc_resp < 0) {
										std::cout << "ERROR RECEIVING PACKET FROM ENCODER" << std::endl;
										return -1;
									}

									// correct output stream parameters
									outpkt->stream_index = ostream->index;
									outpkt->pts = (my_frame_counter * pts_dts_scale);
									outpkt->dts = (my_frame_counter * pts_dts_scale);
									outpkt->duration = pts_dts_scale;
									av_packet_rescale_ts(outpkt, istream->time_base, ostream->time_base);  // really this will do nothing b/c we've enforced that the output timebase must equal the input timebase

									//std::cout << "Wrote pts = " << outpkt->pts << ", dts = " << outpkt->dts << ", my_frame_counter = " << my_frame_counter << " , scale " << pts_dts_scale << ", duration = " << outpkt->duration << std::endl;

									// mux packet into container
									if ((enc_resp = av_interleaved_write_frame(ofmt_ctx, outpkt)) != 0) {
										std::cout << "ERROR WRITING COMPRESSED PACKET: " << enc_resp << std::endl;
										return -1;
									}


									// increment frame counter
									++my_frame_counter;
								}

								// done with this packet, so unreference and free it
								av_packet_unref(outpkt);
								av_packet_free(&outpkt);
							}

						}
	
						// unreference the frame pointers
						av_frame_unref(frame);
						av_frame_unref(frame_cropped);
						//av_frame_free(&frame);

					}
				}

				if (found_frame) {
					if ((int)((my_frame_counter * 100) / num_frames_to_extract) > prev_pct) {
						prev_pct = (int)((my_frame_counter * 100) / num_frames_to_extract);
						printf("\r%4d%% (%8ld/%8ld)", prev_pct, my_frame_counter, num_frames_to_extract);
						fflush(stdout);
					}
				}

			}

			// unreference the packet pointer
			av_packet_unref(inpkt);
		}

		// flush encoder
		// TODO: consolidate this with above to avoid repeat code?
		// TODO: consolidate this with above to avoid repeat code?
		// this might not be a big deal for formats without B-frames
		// since we should get a packet back from the encoder for all I-frames
		// and v210 and ffv1 should have only I-frames
		// see: https://ffmpeg.org/doxygen/trunk/transcoding_8c-example.html
		std::cout << std::endl;
		if( compress_flag ){
			
			// send flush command to encoder
			if (avcodec_send_frame(enc_ctx, NULL) < 0) {
				std::cout << "ERROR SENDING FLUSH PACKET TO ENCODER" << std::endl;
				return -1;
			}

			// read any remaining packets from encoder
			AVPacket* outpkt = av_packet_alloc();;
			int enc_resp = 0;
			while(enc_resp >= 0){
				enc_resp = avcodec_receive_packet(enc_ctx, outpkt);
				if (enc_resp == AVERROR(EAGAIN) || enc_resp == AVERROR_EOF) {
					//std::cout << "NO PACKET AVAILBLE FROM ENCODER" << std::endl;
					break;
				}
				else if (enc_resp < 0) {
					std::cout << "ERROR RECEIVING PACKET FROM ENCODER" << std::endl;
					return -1;
				}

				// correct output stream parameters
				outpkt->stream_index = ostream->index;
				outpkt->pts = my_frame_counter * pts_dts_scale;
				outpkt->dts = my_frame_counter * pts_dts_scale;
				//outpkt->duration = pts_dts_scale;
				av_packet_rescale_ts(outpkt, istream->time_base, ostream->time_base);  // really this will do nothing b/c we've enforced that the output timebase must equal the input timebase

				// mux packet into container
				if ((enc_resp = av_interleaved_write_frame(ofmt_ctx, outpkt)) != 0) {
					std::cout << "ERROR WRITING COMPRESSED PACKET: " << enc_resp << std::endl;
					return -1;
				}

				// increment frame counter
				++my_frame_counter;
			}

			if ((int)((my_frame_counter * 100) / num_frames_to_extract) > prev_pct) {
				prev_pct = (int)((my_frame_counter * 100) / num_frames_to_extract);
				printf("\r%4d%% (%8ld/%8ld)", prev_pct, my_frame_counter, num_frames_to_extract);
				fflush(stdout);
			}

			// done with this packet, so unreference and free it
			av_packet_unref(outpkt);
			av_packet_free(&outpkt);

			// flush buffers
			avcodec_flush_buffers(enc_ctx);
			avcodec_flush_buffers(dec_ctx);
		}

		// write trailer to finish file
		std::cout << std::endl;
		std::cout << "Writing trailer" << std::endl;
		if (av_write_trailer(ofmt_ctx)) {
			std::cout << "ERROR WRITING OUTPUT FILE TRAILER" << std::endl;
			return -1;
		}

		// free output-related memory
		avcodec_free_context(&enc_ctx);
		avio_close(ofmt_ctx->pb);
		avformat_free_context(ofmt_ctx);

		// done processing this clip, stop timer and move on to next one
		auto end_time = std::chrono::high_resolution_clock::now();
		auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time);
		std::cout << "Processed " << my_frame_counter << " frames in " << duration.count() << " seconds" << std::endl;

	}



	// free filter graph
	avfilter_inout_free(&filt_in);
	avfilter_inout_free(&filt_out);
	avfilter_graph_free(&fltgrph);

	// free input-related memory
	avcodec_free_context(&dec_ctx);
	avformat_close_input(&ifmt_ctx);
	avformat_free_context(ifmt_ctx);

	// done
	return 0;
}
