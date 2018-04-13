#include <stream.h>

#include <string.h>
#include <stdlib.h>
#include <iostream>

void startStream(string camera, string address) {
    system(("gst-launch-1.0 -v v4l2src device=" + camera + " ! jpegenc ! rtpjpegpay ! udpsink host=" + address + " port=5000").c_str());
}