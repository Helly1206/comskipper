/* Comskipper = comskippar = comskipparallel
 * Includes a modified version of mpeg2dec.cpp 
 * omitting main()
 * 
 * Copyright 28-03-2015 Helly
 */

#include "mpeg2dec.cpp"

#define CSR_VERSION "0.800"
#define SLEEP_EOF 1
#define FRAME_SF  10
//#define _IDBG

long Finishtime = 0; // epoch time for recording finish
int Frametime = 120; // Timeout to wait for a frame 
bool waitmode=true; // wait for frame if no frame available
bool quitsig=false; // do not signal comskip to quit

void sig_handler(int signo) {
    if (signo == SIGUSR1) {
		::printf("Stop waiting signal flagged\n");
        waitmode=false;
	} else if (signo == SIGINT) { 
		::printf("Interrupt signal flagged\n");
		quitsig=true;   
        exit(0);
    }    
}

void PrintHelp() {
	::printf("\nComskipper is a wrapper around comskip\n");
	::printf("It is able to skip commercials during a recording (with correct ini settings)\n");
	::printf("It uses comskip underneath, so it has the same command line options\n\n");
	::printf("A few extra options are:\n");
	::printf("    Signal comskipper that a recording has finished: SIGUSR1\n");
	::printf("    Signal comskipper to quit: SIGINT\n");
	::printf("    -e, --endtime: The end time of the recording in epoch (default=%ld)\n",Finishtime);
	::printf("    -f, --frameto: Maximum time (in seconds) to wait for new frame (default=%d)\n\n",Frametime);
}

int RemoveArgument(int index, int argc, char **argv) {
	int retval = argc;
	if (index<argc) {
		for (int i = index; i < (argc-1); ++i ) {
			argv[i] = argv[i+1];
		}
		retval--;
	}
	return retval;
}

int ExtractArguments(int argc, char **argv) {
    bool gete=false;
    bool getf=false;
    bool removearg[argc];
    
    for ( int i = 0; i < argc; ++i ) {
		removearg[i]=false;
	}
	
    for ( int i = 0; i < argc; ++i ) {
		if (gete) {
			if (strncmp("-",argv[i],1)) { //not a new command
				Finishtime = atol(argv[i]);
				removearg[i]=true;
		    } else {
				::fprintf(stderr,"Wrong syntax\n");
				PrintHelp();
			}
			gete=false;
		} else if (getf) {
			if (strncmp("-",argv[i],1)) { //not a new command
				Frametime = atoi(argv[i]);
				removearg[i]=true;
		    } else {
				::fprintf(stderr,"Wrong syntax\n");
				PrintHelp();
			}
			getf=false;
		} else if ((!strcmp("-h",argv[i])) || (!strcmp("--help",argv[i]))) {
			PrintHelp();
		} else if ((!strcmp("-e",argv[i])) || (!strcmp("--endtime",argv[i]))) {
			gete=true;
			removearg[i]=true;
		}  else if ((!strcmp("-f",argv[i])) || (!strcmp("--frameto",argv[i]))) {
			getf=true;
			removearg[i]=true;
		}
    }
    
    int retval = argc;
    for ( int i = 0; i < argc; ++i ) {
		if (removearg[i]) {
			retval = RemoveArgument(i-(argc-retval), retval, argv);
		}
	}
    
    return(retval);
}

long long int GetFileSize(VideoState *lis) {
    struct stat st;
    stat(lis->filename, &st);
    return(st.st_size);
}


// ********************** SUBSTITUTES for origninal mpeg2dec.cpp **************************

namespace CS {

void FramesPerSecond::print(bool final) {
	if ( pcs->getVerbose() || csStepping )
		return;

	struct timeval tvEnd;
	double fps;
	double tfps;
	int frames;
	int elapsed;
        int percdone;

	::gettimeofday (&tvEnd, NULL);

	if ( ! frameCounter ) {
		tvStart = tvBeg = tvEnd;
		signal (SIGINT, signal_handler);
	}

	elapsed = (tvEnd.tv_sec - tvBeg.tv_sec) * 100 + (tvEnd.tv_usec - tvBeg.tv_usec) / 10000;
	totalElapsed = (tvEnd.tv_sec - tvStart.tv_sec) * 100 + (tvEnd.tv_usec - tvStart.tv_usec) / 10000;

	if ( final ) {
		if ( totalElapsed )
			tfps = framenum * 100.0 / totalElapsed;
		else
			tfps = 0;

		::fprintf(stderr
					, "\n%d frames decoded in %.2f seconds (%.2f fps)\n"
					, framenum, totalElapsed / 100.0, tfps);
		::fflush(stderr);
		return;
	}

	++frameCounter;

	if ( elapsed < 1000 ) /* only display every 10.00 seconds */
		return;

	tvBeg = tvEnd;
	frames = framenum - lastCount;

	CurrentTime ct((double)framenum / global_video_state->fps);

	fps = frames * 100.0 / elapsed;
	tfps = framenum * 100.0 / totalElapsed;

        percdone = (int) (100.0 * global_video_state->audio_pkt.pos/GetFileSize(global_video_state));

	::fprintf (stderr
			, "%s - %d frames in %.2f sec (%.2f fps [%.2f fps]), %d%%\n"
			, ct.getString().c_str()
			, framenum
			, totalElapsed / 100.0
			, fps, tfps
			, percdone);
	::fflush(stderr);

	lastCount = framenum;
}

} // namespace CS

int main (int argc, char **argv) {
	int result(0);
	long long int position = 0;
	int framesize = 0;
	int framemax = 0;
	bool readframe = false;
	int waitctr = 0;

    	if (signal(SIGUSR1, sig_handler) == SIG_ERR)
        	::fprintf(stderr,"Error signals: can't catch SIGUSR1\n");
    	else if (signal(SIGINT, sig_handler) == SIG_ERR)
        	::fprintf(stderr,"Error signals: can't catch SIGINT\n"); 

	try {
		// Register all formats and codecs
		avcodec_register_all();
		av_register_all();

		// AVFormatContext *pFormatCtx;
		AVPacket pkt1;
		AVPacket *packet = &pkt1;

		int ret;
		// fpos_t fileendpos;

		char *ptr;
		CommercialSkipper commercialSkipper;
		Audio audio(&commercialSkipper);


		// get path to executable
		ptr = argv[0];
		size_t len(::strlen(ptr));

		if ( *ptr == '\"' ) {
			++ptr; //strip off quotation marks
			len = (size_t)(strchr(ptr,'\"') - ptr);
		}

		::strncpy(HomeDir, ptr, len);

		len = (size_t)std::max(0, (int)(::strrchr(HomeDir, DIRSEP) - HomeDir));

		if ( len == 0 ) {
			HomeDir[0] = '.';
			HomeDir[1] = '\0';
		} else {
			HomeDir[len] = '\0';
		}

        	::fprintf (stderr, "Comskipper %s, using ...\n",CSR_VERSION);
		::fprintf (stderr, "Comskip %s.%s, made using avcodec\n"
					, PACKAGE_VERSION, SUBVERSION);

		argc = ExtractArguments(argc,argv);
		commercialSkipper.loadSettings(argc, argv);

		file_open(&commercialSkipper, &audio);

		csRestart = 0;
		framenum = 0;

		DUMP_OPEN

		is = global_video_state;
		is->pcs = &commercialSkipper;
		is->audio = &audio;

		av_log_set_level(AV_LOG_WARNING);
		av_log_set_flags(AV_LOG_SKIP_REPEATED);

		packet = &(is->audio_pkt);

		// main decode loop

		for ( ; ; ) {
			if ( is->quit )
				break;
				
			// IVO: calc frame-sizes
			framesize=(int)(packet->pos - position);
			position=packet->pos;
			if (framesize > framemax) framemax = framesize;	

			// seek stuff goes here
			if ( is->seek_req ) {
				is->seek_req = 0;
				framenum = 0;
				pts_offset = 0.0;
				is->video_clock = 0.0;
				is->audio->resetClock();
				file_close();
				file_open(&commercialSkipper, &audio);
				is->audio->resetSoundFrameCounter();
				initial_pts = 0;
				initial_pts_set = false;
				initial_apts_set = false;
				initial_apts = 0;
				// audio_samples = 0;
			}
				
			if (quitsig) break;
						
			if (waitmode) {
				// Check if to quit waitmode
				if (waitctr > Frametime) { // waiting too long
					::printf("Timeout waiting for frame\n");
					waitmode=false;
				} 
			
				if (Finishtime > 0) {
					if (time(NULL)>Finishtime) { // recording finished
						::printf("Recording finished\n");
						waitmode=false;
					}
				}
#ifdef _IDBG
        	        	::printf("Seek: %lld; Size: %lld; Framesize: %d\n",position,GetFileSize(is),framesize);
#endif
        	        	if (GetFileSize(is)<(position + framemax*FRAME_SF)) {
#ifdef _IDBG
					::printf("Sleep zzzzzzzzzzzzz\n");
#endif
        	            		sleep(SLEEP_EOF); // sleep and try again
        	            		waitctr+=SLEEP_EOF;
        	            		readframe=false;
        	        	} else {
        	            		readframe=true;
        	       	 	}
        	    	} else {
        	        	readframe=true;
        	    	}

			if (readframe) {
				waitctr=0;
				if ( av_read_frame(is->pFormatCtx, packet) < 0 ) {
					break;
				}


				if ( packet->stream_index == is->videoStream )
					video_packet_process(is, packet);
				else if ( packet->stream_index == is->audioStream )
					audio_packet_process(is, packet);

				av_free_packet(packet);

			} // End Else loop (packet>0)
		} // END MAIN LOOP

		commercialSkipper.Debug( 10,"\nParsed %d video frames"
				" and %d audio frames of %4.2f fps\n"
				, framenum
				, is->audio->getSoundFrameCounter()
				, commercialSkipper.getFps());
		commercialSkipper.Debug( 10,"\nMaximum Volume found is %d\n"
				, is->audio->getVolumeMax());

		commercialSkipper.getFramesPerSecond().print(true);

		if ( framenum > 0 ) {
			if ( commercialSkipper.buildMasterCommList() )
        			::fprintf(stderr,"Commercials were found.\n");
			else
        	        	::fprintf(stderr,"Commercials were not found.\n");

        	    	if ( commercialSkipper.getDebugWindowUse() ) {
        	    		doProcessCc = false;
        	            	printf("Close window when done\n");

        	            	DUMP_CLOSE
        	            	commercialSkipper.setTimingUse(false);
        	       	}
		} else {
			result = 1; // failure
		}

		file_close();

	}
	catch ( std::runtime_error &e ) {
		::fprintf(stderr, "%s\n", e.what());
		::exit(6);
	}

	return result;
}
