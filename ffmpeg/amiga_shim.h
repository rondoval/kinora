/* Included ahead of every FFmpeg source file: what libnix's headers leave out. */
#ifndef KINORA_FFMPEG_AMIGA_SHIM_H
#define KINORA_FFMPEG_AMIGA_SHIM_H

int mkstemp(char *name_template);
int usleep(unsigned long microseconds);

#endif
