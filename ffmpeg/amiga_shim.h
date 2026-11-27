/* Included ahead of every FFmpeg source file: what libnix's headers leave out. */
#ifndef KINORA_FFMPEG_AMIGA_SHIM_H
#define KINORA_FFMPEG_AMIGA_SHIM_H
#ifndef __ASSEMBLER__ /* FFmpeg's C flags reach its assembly files too (libavcodec/m68k/) */

int mkstemp(char *name_template);
int usleep(unsigned long microseconds);

#endif
#endif
