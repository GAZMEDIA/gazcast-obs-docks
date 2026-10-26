#pragma once

/* The direct link to the GazCast app (see obs-link.cpp). Call from the UI thread. */
void ObsLinkStart(int port);
void ObsLinkSetPort(int port);
void ObsLinkStop();
