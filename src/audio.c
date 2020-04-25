#define DEBUG
/*
	audio: audio output interface

	This wraps a bit of out123 now, with a layer of resampling using syn123.

	copyright ?-2020 by the mpg123 project - free software under the terms of the LGPL 2.1
	see COPYING and AUTHORS files in distribution or http://mpg123.org
	initially written by Michael Hipp

	Pitching can work in two ways:
	- fixed (native) decoder rate, varied hardware playback rate
	- fixed hardware playback rate, any decoder rate, resampler

	I want to keep the two choices for low-cost hack and proper work. So the
	hardware-varied pitch is combined with native decoder output. But when we
	already employ the NtoM resampler, shouldn't it be used for pitching, too?
	Issue is that it will really not sound so good. Nah, don't complicate things.
	If there is a resampling layer between decoder and output, it's syn123. So
	if you force NtoM decoder, that rate is still subject to hardware pitch.

	To enable software pitching, you decide on a fixed output rate and have the
	proper resampler configured. Then the pitch is just a modification of the
	resampling ratio. But if resampling is not necessary, playing a 44100 Hz
	track with focecd rate of 44100 Hz, the resampler should step aside.

	On the other hand ... for smooth pitching, you'll want the resampler active,
	also if the rate is identical for a moment. But is mpg123's pitch control that
	smooth? The fulll smoothness matters for a true sweep, a pitch ramp. I'll check
	if it matters. holding the pitch key pressed in terminal control.
*/

#include <errno.h>
#include "mpg123app.h"
#include "audio.h"
#include "out123.h"
#include "syn123.h"
#include "common.h"
#include "metaprint.h"
#include "sysutil.h"

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include "debug.h"

static syn123_handle *sh = NULL;
static struct mpg123_fmt outfmt = { .encoding=0, .rate=0, .channels=0 };
static int outch = 0; // currently used number of output channels
// A convoluted way to say outch*4, for semantic clarity.
#define RESAMPLE_FRAMESIZE(ch) ((ch)*MPG123_SAMPLESIZE(MPG123_ENC_FLOAT_32))
#define OUTPUT_FRAMESIZE(ch)   ((ch)*MPG123_SAMPLESIZE(outfmt.encoding))
// Resampler buffers, first for resampling output, then for conversion.
// Instead of sizing them for any possible input rate, I'll try to
// loop in the output wrapper over a fixed buffer size. The syn123 resampler
// can tell me how many samples to feed to avoid exceeding the output buffer.
static char *resample_buffer = NULL;
static char* resample_outbuf = NULL;
static size_t resample_block = 0;
// A good buffer size:
// 1152*48/44.1*2*4 = 10032 ... let's go 16K.
// This should work for final output data, too.
// We'll loop over pieces if the buffer size is not enough for upsampling.
static size_t resample_bytes = 1<<16;
int do_resample = 0;
int do_resample_now = 0; // really apply resampler for current stream.

static int audio_capabilities(out123_handle *ao, mpg123_handle *mh);

void audio_cleanup(void)
{
	if(sh)
		syn123_del(sh);
	if(resample_outbuf)
		free(resample_outbuf);
	if(resample_buffer)
		free(resample_buffer);
}

int audio_setup(out123_handle *ao, mpg123_handle *mh)
{
	do_resample = (param.force_rate > 0 && param.resample);
	resample_block = 0;
	// Settle formats.
	if(audio_capabilities(ao, mh))
		return -1;
	// Prepare resample.
	// Resampling only for forced rate for now. In future, pitching should
	// also be handled.
	if(do_resample)
	{
		int err;
		sh = syn123_new(outfmt.rate, 1, outfmt.encoding, 0, &err);
		if(!sh)
		{
			merror("Cannot initialize syn123: %s\n", syn123_strerror(err));
			return -1;
		}
		resample_buffer = malloc(resample_bytes);
		resample_outbuf = malloc(resample_bytes);
		if(!resample_buffer || !resample_outbuf)
			return -1;
	}
	return 0;
}

int audio_prepare(out123_handle *ao, long rate, int channels, int encoding)
{
	mdebug( "audio_prepare %ld Hz / %ld Hz, %i ch, enc %i"
	,	rate, outfmt.rate, channels, encoding );
	if(do_resample && rate == outfmt.rate)
	{
		do_resample_now = 0;
		debug("disabled resampler for native rate");
	} else if(do_resample)
	{
		do_resample_now = 1;
		// Smooth option could be considered once pitching is implemented with the
		// resampler.The exiting state might fit the coming data if this is two
		// seamless tracks. If not, it's jut the first few samples that differ
		// significantly depending on which data went through the resampler
		// previously.
		int err = syn123_setup_resample( sh, rate, outfmt.rate, channels
		,	(param.resample < 2), 0 );
		if(err)
		{
			merror("failed to set up resampler: %s", syn123_strerror(err));
			return -1;
		}
		outch = channels;
		// We can store a certain ammount of frames in the resampler buffer
		// and the final output buffer after conversion.
		size_t frames = resample_bytes / (
			RESAMPLE_FRAMESIZE(channels) > OUTPUT_FRAMESIZE(channels)
			?	RESAMPLE_FRAMESIZE(channels)
			:	OUTPUT_FRAMESIZE(channels) );
		// Minimum amount of input samples to fill the buffer.
		resample_block = syn123_resample_fillcount(rate, outfmt.rate, frames);
		if(!resample_block)
			return -1; // WTF? No comment.
		mdebug("resampler setup %ld -> %ld, block %zu", rate, outfmt.rate, resample_block);
		rate     = outfmt.rate;
		encoding = outfmt.encoding;
	}
	return out123_start(ao, pitch_rate(rate), channels, encoding);
}

// Loop over blocks with the resampler, think about intflag.
size_t audio_play(out123_handle *ao, void *buffer, size_t bytes)
{
	if(do_resample_now)
	{
		int fs = RESAMPLE_FRAMESIZE(outch);
		size_t pcmframes = bytes/fs;
		size_t done = 0;
		while(pcmframes && !intflag)
		{
			size_t block = resample_block > pcmframes
			?	pcmframes
			:	resample_block;
			size_t oblock = syn123_resample( sh, (float*)resample_buffer
			,	(float*)((char*)buffer+done), block );
			if(!oblock)
				break;
			size_t obytes = 0;
			if(syn123_conv( resample_outbuf, outfmt.encoding, resample_bytes
			,	resample_buffer, MPG123_ENC_FLOAT_32, oblock*fs
			,	&obytes, NULL, sh ))
				break;
			size_t oplay = out123_play(ao, resample_outbuf, obytes);
			if(oplay < obytes)
			{
				// Need to translate that. How many input samples got played,
				// actually? A bit of roundoff error doesn't hurt, so let's just
				// wing it. Close is enough.
				size_t iframes = (size_t)((double)oplay/obytes*block);
				while(iframes >= block)
					--iframes;
				done += iframes*fs;
				break;
			}
			pcmframes -= block;
			done      += block*fs;
		}
		return done;
	}
	else
		return out123_play(ao, buffer, bytes);
}

mpg123_string* audio_enclist(void)
{
	int i;
	mpg123_string *list;
	size_t enc_count = 0;
	const int *enc_codes = NULL;

	/* Only the encodings supported by libmpg123 build
	   Those returned by out123_enc_list() are a superset. */
	mpg123_encodings(&enc_codes, &enc_count);
	if((list = malloc(sizeof(*list))))
		mpg123_init_string(list);
	/* Further calls to mpg123 string lib are hardened against NULL. */
	for(i=0;i<enc_count;++i)
	{
		if(i>0)
			mpg123_add_string(list, " ");
		mpg123_add_string(list, out123_enc_name(enc_codes[i]));
	}
	return list;
}

static void capline(mpg123_handle *mh, long rate, struct mpg123_fmt *outfmt)
{
	int enci;
	const int  *encs;
	size_t      num_encs;
	mpg123_encodings(&encs, &num_encs);
	fprintf(stderr," %5ld |", pitch_rate(outfmt ? outfmt->rate : rate));
	for(enci=0; enci<num_encs; ++enci)
	{
		int fmt = outfmt
		?	(encs[enci] == outfmt->encoding ? outfmt->channels : 0)
		:	mpg123_format_support(mh, rate, encs[enci]);
		switch(fmt)
		{
			case MPG123_MONO:               fprintf(stderr, "   M  "); break;
			case MPG123_STEREO:             fprintf(stderr, "   S  "); break;
			case MPG123_MONO|MPG123_STEREO: fprintf(stderr, "  M/S "); break;
			default:                        fprintf(stderr, "      ");
		}
	}
	fprintf(stderr, "\n");
}

void print_capabilities(out123_handle *ao, mpg123_handle *mh)
{
	int r,e;
	const long *rates;
	size_t      num_rates;
	const int  *encs;
	size_t      num_encs;
	char *name;
	char *dev;
	out123_driver_info(ao, &name, &dev);
	mpg123_rates(&rates, &num_rates);
	mpg123_encodings(&encs, &num_encs);
	fprintf(stderr,"\nAudio driver: %s\nAudio device: ", name);
	print_outstr(stderr, dev, 0, stderr_is_term);
	fprintf(stderr, "\n");
	fprintf( stderr, "%s", "Audio capabilities:\n"
		"(matrix of [S]tereo or [M]ono support for sample format and rate in Hz)\n"
		"\n"
		" rate  |" );
	for(e=0;e<num_encs;e++)
	{
		const char *encname = out123_enc_name(encs[e]);
		fprintf(stderr," %4s ", encname ? encname : "???");
	}

	fprintf(stderr,"\n -------");
	for(e=0;e<num_encs;e++) fprintf(stderr,"------");

	fprintf(stderr, "\n");
	for(r=0; r<num_rates; ++r) capline(mh, rates[r], NULL);

	if(param.force_rate)
	{
		fprintf(stderr," -------");
		for(e=0;e<num_encs;e++) fprintf(stderr,"------");
		fprintf(stderr, "\n");
		if(do_resample)
			capline(mh, 0, &outfmt);
		else
			capline(mh, param.force_rate, NULL);
	}
	fprintf(stderr,"\n");
	if(do_resample)
		fprintf( stderr, "%s\n%s\n"
		,	"Resampler configured. Decoding to f32 as intermediate if needed."
		,	"Resampler output format is in the last line." );
	else if(param.force_rate)
		fprintf( stderr, "%s\n"
		,	"Decoder rate forced. Resulting format support shown in last line." );
}

/* Quick-shot paired table setup with remembering search in it.
   this is for storing pairs of output sampling rate and decoding
   sampling rate. */
struct ratepair { long a; long b; };

long brate(struct ratepair *table, long arate, int count, int *last)
{
	int i = 0;
	int j;
	for(j=0; j<2; ++j)
	{
		i = i ? 0 : *last;
		for(; i<count; ++i) if(table[i].a == arate)
		{
			*last = i;
			return table[i].b;
		}
	}
	return 0;
}

// Return one of the given list of encodings matching the mask,
// in order. Zero if none.
static int match_enc(int mask, const int *enc_list, size_t enc_count)
{
	int enc = 0;
	for(size_t i=0; i<enc_count; ++i)
	{
		if((enc_list[i] & mask) == enc_list[i])
		{
			enc = enc_list[i];
			break;
		}
	}
	return enc;
}

/* This uses the currently opened audio device, queries its caps.
   In case of buffered playback, this works _once_ by querying the buffer for the caps before entering the main loop. */
static int audio_capabilities(out123_handle *ao, mpg123_handle *mh)
{
	int force_fmt = 0;
	size_t ri;
	/* Pitching introduces a difference between decoder rate and playback rate. */
	long decode_rate;
	const long *rates;
	long *outrates;
	struct ratepair *unpitch;
	struct mpg123_fmt *outfmts = NULL;
	int fmtcount;
	size_t num_rates, rlimit;
	long ntom_rate = do_resample ? 0 : param.force_rate;
	outfmt.rate = param.force_rate;
	outfmt.channels = 0;
	outfmt.encoding = 0;

	debug("audio_capabilities");
	mpg123_rates(&rates, &num_rates);

	mpg123_format_none(mh); /* Start with nothing. */

	if(do_resample && param.verbose > 2)
		fprintf( stderr
		,	"Note: decoder always forced to %s encoding for resampler\n"
		,	out123_enc_name(MPG123_ENC_FLOAT_32) );

	if(param.force_encoding != NULL)
	{
		if(!param.quiet)
			fprintf(stderr, "Note: forcing output encoding %s\n", param.force_encoding);

		force_fmt = out123_enc_byname(param.force_encoding);
		if(!force_fmt)
		{
			mpg123_string fs;
			mpg123_init_string(&fs);
			outstr(&fs, param.force_encoding, 0, stderr_is_term);
			error1("Failed to find an encoding to match requested \"%s\"!\n"
			,	MPGSTR(fs));
			mpg123_free_string(&fs);
			return -1; /* No capabilities at all... */
		}
		else if(param.verbose > 2)
			fprintf(stderr, "Note: forcing encoding code 0x%x (%s)\n"
			,	(unsigned)force_fmt, out123_enc_name(force_fmt));
	}

//TODO: also enable downsampled decoding to avoid resampler decimation steps
//How could I achieve that? Provide a target rate as libmpg123 parametger to indicate
//that I'd like the smallest rate that's larger than that and the native rate.
//A bit silly but this is why it makes sense to integrate the resampler with
//libmpg123 usage.
	if(do_resample)
	{
		if(param.pitch != 0)
			fprintf(stderr, "WARNING: interaction of pitch and resampler not yet settled\n");
		// If really doing the extra resampling, output will always run with
		// this setup, regardless of decoder.
		int enc1 = out123_encodings(ao, param.force_rate, 1);
		int enc2 = out123_encodings(ao, param.force_rate, 2);
		if(force_fmt)
		{
			enc1 &= force_fmt;
			enc2 &= force_fmt;
		}
		mdebug("enc mono=0x%x stereo=0x%x", (unsigned)enc1, (unsigned)enc2);
		if(!enc1 && !enc2)
		{
			error("Output device does not support forced rate and/or encoding.");
			return -1;
		} else if(enc1 && enc2)
		{
			// Should be the normal case: Mono and Stereo with the same formats.
			// We'll store the common subset, which should normally be all.
			outfmt.encoding = enc1 & enc2;
			outfmt.channels = MPG123_MONO|MPG123_STEREO;
			if(!outfmt.encoding)
			{
				error("No common decodings for mono and stereo output. Too weird.");
				return -1;
			}
		} else if(enc1)
		{ // Mono only.
			outfmt.encoding = enc1;
			outfmt.channels = 1;
		} else
		{ // Stereo only.
			outfmt.encoding = enc2;
			outfmt.channels = 2;
		}
		int pref_enc[] = { MPG123_ENC_FLOAT_32
		,	MPG123_ENC_SIGNED_32, MPG123_ENC_UNSIGNED_32
		,	MPG123_ENC_SIGNED_24, MPG123_ENC_UNSIGNED_24
		,	MPG123_ENC_SIGNED_16, MPG123_ENC_UNSIGNED_16 };
		int nenc = match_enc(outfmt.encoding, pref_enc, sizeof(pref_enc)/sizeof(int));
		if(!nenc)
		{
			const int *encs;
			size_t num_encs;
			mpg123_encodings(&encs, &num_encs);
			nenc = match_enc(outfmt.encoding, encs, num_encs);
		}
		if(nenc)
			outfmt.encoding = nenc;
		else
		{
			merror( "Found no encoding to match mask 0x%08x."
			,	(unsigned int)outfmt.encoding );
			if(force_fmt)
				error("Perhaps your forced output encoding is not supported.");
			return -1;
		}
	}

	/* Lots of preparation of rate lists. */
	rlimit = ntom_rate > 0 ? num_rates+1 : num_rates;
	outrates = malloc(sizeof(*rates)*rlimit);
	unpitch  = malloc(sizeof(*unpitch)*rlimit);
	if(!outrates || !unpitch)
	{
		error("DOOM");
		return -1;
	}
	for(ri = 0; ri<rlimit; ri++)
	{
		decode_rate = ri < num_rates ? rates[ri] : ntom_rate;
		outrates[ri] = pitch_rate(decode_rate);
		unpitch[ri].a = outrates[ri];
		unpitch[ri].b = decode_rate;
	}
	/* Actually query formats possible with given rates. */
	fmtcount = out123_formats(ao, outrates, rlimit, 1, 2, &outfmts);
	free(outrates);
	if(fmtcount > 0)
	{
		int fi;
		int unpitch_i = 0;
		if(param.verbose > 1 && outfmts[0].encoding > 0)
		{
			const char *encname = out123_enc_name(outfmts[0].encoding);
			fprintf(stderr, "Note: default format %li Hz, %i channels, %s\n"
			,	outfmts[0].rate, outfmts[0].channels
			,	encname ? encname : "???" );
		}
		for(fi=1; fi<fmtcount; ++fi)
		{
			int fmts = outfmts[fi].encoding;
			if(param.verbose > 2)
				fprintf( stderr
				,	"Note: output support for %li Hz, %i channels: 0x%x\n"
				,	outfmts[fi].rate, outfmts[fi].channels, outfmts[fi].encoding );
			if(force_fmt)
			{ /* Filter for forced encoding. */
				if((fmts & force_fmt) == force_fmt)
					fmts = force_fmt;
				else /* Nothing else! */
					fmts = 0;
			}
			// Support the resampler or native playback. Condition for the resampler
			// to work is decoding to float and keeping a channel count compatible
			// with configured output (in a case that might differ for various encodings).
			long decode_rate = brate(unpitch, outfmts[fi].rate, rlimit, &unpitch_i);
			if(do_resample && decode_rate != outfmt.rate)
			{
				// Only enable float outupt for resampler if needed and channel
				// count supported for real output format.
				fmts = 0;
				if((outfmts[fi].channels & outfmt.channels) == outfmts[fi].channels)
					fmts = MPG123_ENC_FLOAT_32;
			}
			mpg123_format(mh, decode_rate, outfmts[fi].channels, fmts);
		}
	}
	free(outfmts);
	free(unpitch);

	if(param.verbose > 1) print_capabilities(ao, mh);

	return 0;
}

int set_pitch(mpg123_handle *fr, out123_handle *ao, double new_pitch)
{
	double old_pitch = param.pitch;
	long rate;
	int channels, format;
	int smode = 0;

	/* Be safe, check support. */
	if(mpg123_getformat(fr, &rate, &channels, &format) != MPG123_OK)
	{
		/* We might just not have a track handy. */
		error("There is no current audio format, cannot apply pitch. This might get fixed in future.");
		return 0;
	}

	if(do_resample)
	{
		rate = outfmt.rate;
		format = outfmt.encoding;
	}

	param.pitch = new_pitch;
	if(param.pitch < -0.99) param.pitch = -0.99;

	if(channels == 1) smode = MPG123_MONO;
	if(channels == 2) smode = MPG123_STEREO;

	out123_stop(ao);
	/* Remember: This takes param.pitch into account. */
	audio_capabilities(ao, fr);
	if(!(mpg123_format_support(fr, rate, format) & smode))
	{

		/* Note: When using --pitch command line parameter, you can go higher
		   because a lower decoder sample rate is automagically chosen.
		   Here, we'd need to switch decoder rate during track... good? */
		error("Reached a hardware limit there with pitch!");
		param.pitch = old_pitch;
		audio_capabilities(ao, fr);
	}
	return out123_start(ao, pitch_rate(rate), channels, format);
}

int set_mute(out123_handle *ao, int mutestate)
{
	return out123_param( ao
	,	mutestate ? OUT123_ADD_FLAGS : OUT123_REMOVE_FLAGS
	,	OUT123_MUTE, 0, NULL );
}
