#include "usb_keyboard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static UsbDsInterface iface;
static UsbDsEndpoint ep;
static UsbState bus_state=UsbState_Detached;
static struct usb_control_setup setup;
static uint32_t sequence,ep_urb,in_urb,out_urb;
static unsigned ep_size=8,stalls,cancelled,closed,fail_append;
static unsigned completed_status=3;
static uint8_t input[4096],key_report[8],configs[3][64];
static void *output;
static size_t input_size,output_size,config_sizes[3];
static bool in_queued,out_queued;
static bool in_data_completed;

uint32_t hosversionGet(void) { return 22<<16; }
bool hosversionBefore(unsigned a,unsigned b,unsigned c) { (void)b;(void)c;return a>22; }
int appletGetAppletType(void) { return 0; }
void armDCacheFlush(void *p,size_t n) { (void)p;(void)n; }
Result eventWait(Event *e,uint64_t ns) { (void)ns;return e->fired?0:1; }
Result eventClear(Event *e) { e->fired=false;return 0; }
Result usbDsInitialize(void) { memset(&iface,0,sizeof(iface));memset(&ep,0,sizeof(ep));memset(config_sizes,0,sizeof(config_sizes));return 0; }
void usbDsExit(void) { closed++; }
Result usbDsClearDeviceData(void) { return 0; }
Result usbDsAddUsbLanguageStringDescriptor(uint8_t *idx,const uint16_t *s,uint16_t n) { (void)s;assert(n==1);*idx=0;return 0; }
Result usbDsAddUsbStringDescriptor(uint8_t *idx,const char *s) { static uint8_t next=1;assert(s);*idx=next++;return 0; }
Result usbDsSetUsbDeviceDescriptor(UsbDeviceSpeed speed,struct usb_device_descriptor *d) {
    assert(d->bDeviceClass==0 && d->bNumConfigurations==1 && d->bLength==18);
    assert(d->idVendor==0x1209 && d->idProduct==1);
    assert(d->bMaxPacketSize0==(speed==UsbDeviceSpeed_Super?9:64));return 0;
}
Result usbDsSetBinaryObjectStore(const void *p,size_t n) { const uint8_t *b=p;assert(n==22 && b[2]==22);return 0; }
Result usbDsRegisterInterfaceEx(UsbDsInterface **out,uint8_t number) { assert(number==0);*out=&iface;return 0; }
Result usbDsInterface_AppendConfigurationData(UsbDsInterface *i,UsbDeviceSpeed s,const void *p,size_t n) {
    assert(i==&iface);if(fail_append) return 99;
    unsigned index=s-UsbDeviceSpeed_Full;assert(index<3 && config_sizes[index]+n<=64);
    memcpy(configs[index]+config_sizes[index],p,n);config_sizes[index]+=n;return 0;
}
Result usbDsInterface_RegisterEndpoint(UsbDsInterface *i,UsbDsEndpoint **out,uint8_t address) { assert(i==&iface && address==0x81);*out=&ep;return 0; }
Result usbDsInterface_EnableInterface(UsbDsInterface *i) { assert(i==&iface);return 0; }
Result usbDsEnable(void) { return 0; }
Result usbDsInterface_StallCtrl(UsbDsInterface *i) { assert(i==&iface);stalls++;return 0; }
Result usbDsInterface_CtrlInPostBufferAsync(UsbDsInterface *i,void *p,size_t n,uint32_t *id) {
    assert(i==&iface && ((uintptr_t)p%4096)==0);assert(n<=4096);
    /* Regression from the hardware log: OUT-before-IN rejects the IN request. */
    if(out_queued && output_size==0) return 0x00025c8c;
    memcpy(input,p,n);input_size=n;in_urb=*id=++sequence;in_queued=true;return 0;
}
Result usbDsInterface_CtrlOutPostBufferAsync(UsbDsInterface *i,void *p,size_t n,uint32_t *id) {
    assert(i==&iface && ((uintptr_t)p%4096)==0 && n<=1);
    if(n==0 && (!in_data_completed || in_queued)) return 0x00025c8c;
    output=p;output_size=n;out_urb=*id=++sequence;out_queued=true;return 0;
}
Result usbDsInterface_GetSetupPacket(UsbDsInterface *i,void *p,size_t n) { assert(i==&iface && n==8);in_data_completed=false;memcpy(p,&setup,n);return 0; }
Result usbDsEndpoint_Cancel(UsbDsEndpoint *e) { assert(e==&ep);cancelled++;return 0; }
static void report(UsbDsReportData *r,uint32_t id,unsigned size) {
    memset(r,0,sizeof(*r));r->report_count=1;r->report[0]=(UsbDsReportEntry){id,size,size,completed_status};
}
Result usbDsEndpoint_GetReportData(UsbDsEndpoint *e,UsbDsReportData *r) { assert(e==&ep);report(r,ep_urb,ep_size);return 0; }
Result usbDsGetState(UsbState *s) { *s=bus_state;return 0; }
Result usbDsInterface_GetCtrlInReportData(UsbDsInterface *i,UsbDsReportData *r) { assert(i==&iface);in_queued=false;in_data_completed=input_size>0;report(r,in_urb,(unsigned)input_size);return 0; }
Result usbDsInterface_GetCtrlOutReportData(UsbDsInterface *i,UsbDsReportData *r) { assert(i==&iface);out_queued=false;report(r,out_urb,(unsigned)output_size);return 0; }
Result usbDsParseReportData(UsbDsReportData *r,uint32_t id,uint32_t *requested,uint32_t *transferred) {
    if(r->report_count!=1 || r->report[0].id!=id || r->report[0].urb_status!=3) return 7;
    if(requested) *requested=r->report[0].requestedSize;
    if(transferred) *transferred=r->report[0].transferredSize;return 0;
}
Result usbDsEndpoint_PostBufferAsync(UsbDsEndpoint *e,void *p,size_t n,uint32_t *id) {
    assert(e==&ep && n==8 && ((uintptr_t)p%4096)==0);memcpy(key_report,p,8);ep_urb=*id=++sequence;return 0;
}
static void request(unsigned type,unsigned req,unsigned value,unsigned index,unsigned length,uint64_t now) {
    /* Finish data then status before the host starts a subsequent SETUP. */
    for(unsigned stage=0;stage<3 && (in_queued || out_queued);stage++) {
        if(in_queued) iface.CtrlInCompletionEvent.fired=true;
        if(out_queued) iface.CtrlOutCompletionEvent.fired=true;
        keyboard_poll(now);
    }
    setup=(struct usb_control_setup){(uint8_t)type,(uint8_t)req,(uint16_t)value,(uint16_t)index,(uint16_t)length};
    iface.SetupEvent.fired=true;keyboard_poll(now);
}
int main(void) {
    assert(keyboard_init()==0 && keyboard_status()->initialized);
    assert(!keyboard_status()->leds_known);
    for(unsigned s=0;s<3;s++) {
        assert(config_sizes[s]==(s==2?31:25));
        assert(configs[s][5]==3 && configs[s][6]==1 && configs[s][7]==1);
        assert(configs[s][9]==9 && configs[s][10]==0x21);
        assert(configs[s][18]==7 && configs[s][20]==0x81 && configs[s][21]==3 && configs[s][22]==8);
        assert(configs[s][24]==(s==0?1:4)); /* 1ms interrupt polling request */
    }
    assert(keyboard_send(0,4,0)!=0);
    bus_state=UsbState_Configured;keyboard_poll(1);assert(!keyboard_status()->ready);
    request(0x81,6,0x2200,0,255,2);assert(input_size==63 && input[0]==5 && keyboard_status()->report_requests==1);
    assert(in_queued && !out_queued); /* no OUT status before IN data completes */
    unsigned descriptor_size=(unsigned)input_size;
    /* Parse HID items and independently verify report sizes and absence of IDs. */
    unsigned bits[3]={0},size=0,count=0;
    for(size_t p=0;p<input_size;) {
        unsigned tag=input[p++],n=tag&3;if(n==3)n=4;unsigned value=0;
        for(unsigned j=0;j<n;j++) value|=(unsigned)input[p++]<<(j*8);
        if(tag==0x75) size=value;if(tag==0x95) count=value;
        assert(tag!=0x85);
        if((tag&0xfc)==0x80) bits[0]+=size*count;
        if((tag&0xfc)==0x90) bits[1]+=size*count;
    }
    assert(bits[0]==64 && bits[1]==8);
    iface.CtrlInCompletionEvent.fired=true;keyboard_poll(3);
    assert(!in_queued && out_queued && output_size==0);
    assert(!keyboard_status()->descriptor_valid && !keyboard_status()->ready);
    iface.CtrlOutCompletionEvent.fired=true;keyboard_poll(4);
    assert(keyboard_status()->descriptor_valid && !keyboard_status()->ready);
    keyboard_poll(302);assert(keyboard_status()->ready);
    request(0x81,6,0x2100,0,9,400);assert(input_size==9 && input[7]==descriptor_size);
    request(0x81,6,0x2200,0,5,401);assert(input_size==5);
    request(0x21,11,0,0,0,402);assert(input_size==0 && keyboard_status()->protocol==0);
    request(0xa1,3,0,0,1,403);assert(input_size==1 && input[0]==0);
    request(0x21,10,0x0500,0,0,404);assert(keyboard_status()->idle==5);
    assert(keyboard_idle_due(405));
    request(0x21,9,0x0200,0,1,406);assert(output && output_size==1);*(uint8_t*)output=2;
    iface.CtrlOutCompletionEvent.fired=true;keyboard_poll(407);assert(keyboard_status()->leds==2 && keyboard_status()->leds_known && input_size==0);
    unsigned before=stalls;request(0x81,6,0x2200,1,10,408);assert(stalls==before+1);
    assert(keyboard_send(2,11,500)==0 && key_report[0]==2 && key_report[2]==11);
    assert(keyboard_send(0,0,501)!=0);
    ep.CompletionEvent.fired=true;keyboard_poll(502);assert(keyboard_status()->reports_completed==1 && !keyboard_status()->transfer_pending);
    request(0xa1,1,0x0100,0,8,503);assert(input_size==8 && input[2]==11);
    assert(keyboard_send(0,0,504)==0);ep_size=7;ep.CompletionEvent.fired=true;keyboard_poll(505);
    assert(keyboard_status()->reports_failed==1);
    ep_size=8;assert(keyboard_send(0,4,510)==0);keyboard_poll(3011);
    assert(!keyboard_status()->transfer_pending && keyboard_status()->reports_failed==2 && cancelled);
    assert(keyboard_send(0,4,3100)==0);bus_state=UsbState_Default;keyboard_poll(3101);
    assert(!keyboard_status()->ready && !keyboard_status()->transfer_pending);
    assert(!keyboard_status()->descriptor_valid && keyboard_status()->bus_resets==1);
    assert(!keyboard_status()->leds_known && keyboard_status()->leds==0);
    bus_state=UsbState_Configured;keyboard_poll(3102);keyboard_poll(3500);
    assert(!keyboard_status()->ready); /* a bus reset requires a fresh descriptor handshake */
    request(0x81,6,0x2200,0,63,3501);
    iface.CtrlInCompletionEvent.fired=true;keyboard_poll(3502);
    assert(!keyboard_status()->ready && out_queued);
    iface.CtrlOutCompletionEvent.fired=true;keyboard_poll(3503);assert(keyboard_status()->ready);
    bus_state=UsbState_Suspended;keyboard_poll(3600);
    assert(!keyboard_status()->ready && keyboard_status()->descriptor_valid);
    bus_state=UsbState_Configured;keyboard_poll(3601);keyboard_poll(3902);
    assert(keyboard_status()->ready); /* suspend/resume does not re-enumerate the device */
    request(0x81,6,0x2200,0,63,4000);
    completed_status=5;iface.CtrlInCompletionEvent.fired=true;keyboard_poll(4001);
    assert(!out_queued && !keyboard_status()->ready && keyboard_status()->error!=0);
    completed_status=3; /* failed IN data must never advance to OUT status */
    keyboard_exit();assert(closed==1);
    fail_append=1;assert(keyboard_init()==99);assert(!keyboard_status()->initialized && closed==2);keyboard_exit();
    puts("PASS: USB descriptors, IN data completion BEFORE OUT status, readiness handshake and reset, LEDs, boot protocol, transfer acknowledgements, timeout and init cleanup.");return 0;
}
