#ifndef TEST_STUB_CONFIG_NETWORK_H
#define TEST_STUB_CONFIG_NETWORK_H
#ifndef TEST_WIFI_H
#define TEST_WIFI_H
#define WIFI_SUCCESS 0
#define WIFI_CONNECTED 1
#define WIFI_STATE_AVALIABLE 1
#define WIFI_HOTSPOT_ACTIVE 1
#define VENDOR_ID_WIFI_SSID 1
#define VENDOR_ID_WIFI_PASSWD 2
#define VENDOR_ID_NET_IP 3
#define VENDOR_ID_NET_GW 4
#define VENDOR_ID_NET_MASK 5
#define VENDOR_ID_WIFI_MODE 6
#define VENDOR_ID_WIFI_ROUTE_SSID 7
#define VENDOR_ID_WIFI_ROUTE_PASSWD 8
typedef struct {int connState,rssi,frequency;unsigned int ipAddress;char ssid[33];} WifiLinkedInfo;
int GetLinkedInfo(WifiLinkedInfo*);int IsHotspotActive(void);int IsWifiActive(void);
int SetWifiModeOff(void);int SetApModeOn(void);int SetWifiModeOn(void);int WifiDisconnect(int);int WifiConnect(unsigned char*,unsigned char*);
void FlashInit(void);int VendorSet(int,unsigned char*,unsigned int);
#endif

#endif
