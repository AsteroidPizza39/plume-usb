#include "ui.h"
#include "view.h"
#include "pace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save(View *view,const char *name) {
    uint32_t *pixels=calloc(1280*720,4);
    view_render(view,pixels,1280*4);
    FILE *out=fopen(name,"wb");
    fprintf(out,"P6\n1280 720\n255\n");
    for(int i=0;i<1280*720;i++) { unsigned char rgb[]={pixels[i]&255,(pixels[i]>>8)&255,(pixels[i]>>16)&255};fwrite(rgb,1,3,out); }
    fclose(out);free(pixels);
}
int main(void) {
    if(!ui_init()) return 1;
    View view={0};view.enter_pause_ms=200;view.caps_state=-1;strcpy(view.usb_label,"USB keyboard connected");view.usb_ready=true;view.usb_initialized=true;
    save(&view,"home-light.ppm");view.dark=true;save(&view,"home-dark.ppm");
    view.caps_state=1;view.pace=PACE_VERY_FAST;save(&view,"home-veryfast.ppm");
    view.caps_pending=true;view.pace=PACE_TURBO;save(&view,"home-turbo.ppm");view.pace=PACE_NORMAL;
    view.caps_pending=false;view.caps_state=0;
    view.screen=SCREEN_FILES;view.file_rows=8;view.file_count=36;view.selected=3;
    strcpy(view.path,"sdmc:/texts/");
    for(int i=0;i<8;i++) { snprintf(view.files[i],256,"%s %d - sample.txt",i<2?"Folder":"File",i+1);view.directories[i]=i<2; }
    save(&view,"files-dark.ppm");
    view.screen=SCREEN_CONFIRM;view.pace=PACE_TURBO;strcpy(view.name,"switch2-test.txt");view.characters=157;view.lines=2;
    strcpy(view.preview,"Hello.\nUSB keyboard test: 0123456789.\nAccent test: é è à ç ù.");
    save(&view,"confirm-dark.ppm");view.dark=false;save(&view,"confirm-light.ppm");
    view.screen=SCREEN_TRANSFER;view.sending=true;view.done=51;view.total=157;save(&view,"sending-light.ppm");
    view.screen=SCREEN_DIAGNOSTICS;
    for(int i=0;i<12;i++) snprintf(view.diagnostics[i],144,"USB status %d / result: 0x00000000",i+1);
    save(&view,"diagnostics-light.ppm");
    view.screen=SCREEN_RESULT;view.error=true;
    strcpy(view.message,"Unsupported character U+1F600 in AZERTY FR at position 25. Nothing was sent.");
    save(&view,"error-light.ppm");ui_shutdown();return 0;
}
