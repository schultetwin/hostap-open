/*
 * Wi-Fi Protected Setup - External Registrar
 * Copyright (c) 2009, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "includes.h"

#include "common.h"
#include "base64.h"
#include "uuid.h"
#include "eloop.h"
#include "httpread.h"
#include "http_client.h"
#include "http_server.h"
#include "upnp_xml.h"
#include "wps_i.h"
#include "wps_upnp.h"
#include "wps_upnp_i.h"


/* TODO:
 * send notification of new AP device with wpa_msg
 * re-send notifications with wpa_msg if ER re-started (to update wpa_gui-qt4)
 * (also re-send SSDP M-SEARCH in this case to find new APs)
 * parse UPnP event messages
 */

static void wps_er_ap_timeout(void *eloop_data, void *user_ctx);
static void wps_er_sta_timeout(void *eloop_data, void *user_ctx);


struct wps_er_sta {
	struct wps_er_sta *next;
	struct wps_er_ap *ap;
	u8 addr[ETH_ALEN];
	u16 config_methods;
	u8 uuid[WPS_UUID_LEN];
	u8 pri_dev_type[8];
	u16 dev_passwd_id;
	int m1_received;
	char *manufacturer;
	char *model_name;
	char *model_number;
	char *serial_number;
	char *dev_name;
	struct wps_data *wps;
	struct http_client *http;
};

struct wps_er_ap {
	struct wps_er_ap *next;
	struct wps_er *er;
	struct wps_er_sta *sta; /* list of STAs/Enrollees using this AP */
	struct in_addr addr;
	char *location;
	struct http_client *http;

	char *friendly_name;
	char *manufacturer;
	char *manufacturer_url;
	char *model_description;
	char *model_name;
	char *model_number;
	char *model_url;
	char *serial_number;
	char *udn;
	char *upc;

	char *scpd_url;
	char *control_url;
	char *event_sub_url;

	int subscribed;
	unsigned int id;
};

struct wps_er {
	struct wps_context *wps;
	char ifname[17];
	char *mac_addr_text; /* mac addr of network i.f. we use */
	u8 mac_addr[ETH_ALEN]; /* mac addr of network i.f. we use */
	char *ip_addr_text; /* IP address of network i.f. we use */
	unsigned ip_addr; /* IP address of network i.f. we use (host order) */
	int multicast_sd;
	int ssdp_sd;
	struct wps_er_ap *ap;
	struct http_server *http_srv;
	int http_port;
	unsigned int next_ap_id;
};


static struct wps_er_sta * wps_er_sta_get(struct wps_er_ap *ap, const u8 *addr)
{
	struct wps_er_sta *sta = ap->sta;
	while (sta) {
		if (os_memcmp(sta->addr, addr, ETH_ALEN) == 0)
			return sta;
		sta = sta->next;
	}
	return NULL;
}


static void wps_er_sta_free(struct wps_er_sta *sta)
{
	if (sta->wps)
		wps_deinit(sta->wps);
	os_free(sta->manufacturer);
	os_free(sta->model_name);
	os_free(sta->model_number);
	os_free(sta->serial_number);
	os_free(sta->dev_name);
	http_client_free(sta->http);
	eloop_cancel_timeout(wps_er_sta_timeout, sta, NULL);
	os_free(sta);
}


static void wps_er_sta_remove_all(struct wps_er_ap *ap)
{
	struct wps_er_sta *prev, *sta;

	sta = ap->sta;
	ap->sta = NULL;

	while (sta) {
		prev = sta;
		sta = sta->next;
		wps_er_sta_free(prev);
	}
}


static struct wps_er_ap * wps_er_ap_get(struct wps_er *er,
					struct in_addr *addr)
{
	struct wps_er_ap *ap;
	for (ap = er->ap; ap; ap = ap->next) {
		if (ap->addr.s_addr == addr->s_addr)
			break;
	}
	return ap;
}


static struct wps_er_ap * wps_er_ap_get_id(struct wps_er *er, unsigned int id)
{
	struct wps_er_ap *ap;
	for (ap = er->ap; ap; ap = ap->next) {
		if (ap->id == id)
			break;
	}
	return ap;
}


static void wps_er_ap_free(struct wps_er *er, struct wps_er_ap *ap)
{
	/* TODO: if ap->subscribed, unsubscribe from events if the AP is still
	 * alive */
	wpa_printf(MSG_DEBUG, "WPS ER: Removing AP entry for %s (%s)",
		   inet_ntoa(ap->addr), ap->location);
	eloop_cancel_timeout(wps_er_ap_timeout, er, ap);
	os_free(ap->location);
	http_client_free(ap->http);

	os_free(ap->friendly_name);
	os_free(ap->manufacturer);
	os_free(ap->manufacturer_url);
	os_free(ap->model_description);
	os_free(ap->model_name);
	os_free(ap->model_number);
	os_free(ap->model_url);
	os_free(ap->serial_number);
	os_free(ap->udn);
	os_free(ap->upc);

	os_free(ap->scpd_url);
	os_free(ap->control_url);
	os_free(ap->event_sub_url);

	wps_er_sta_remove_all(ap);

	os_free(ap);
}


static void wps_er_ap_timeout(void *eloop_data, void *user_ctx)
{
	struct wps_er *er = eloop_data;
	struct wps_er_ap *ap = user_ctx;
	wpa_printf(MSG_DEBUG, "WPS ER: AP advertisement timed out");
	wps_er_ap_free(er, ap);
}


static void wps_er_http_subscribe_cb(void *ctx, struct http_client *c,
				     enum http_client_event event)
{
	struct wps_er_ap *ap = ctx;

	switch (event) {
	case HTTP_CLIENT_OK:
		wpa_printf(MSG_DEBUG, "WPS ER: Subscribed to events");
		break;
	case HTTP_CLIENT_FAILED:
	case HTTP_CLIENT_INVALID_REPLY:
	case HTTP_CLIENT_TIMEOUT:
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to subscribe to events");
		break;
	}
	http_client_free(ap->http);
	ap->http = NULL;
}


static void wps_er_subscribe(struct wps_er_ap *ap)
{
	struct wpabuf *req;
	struct sockaddr_in dst;
	char *url, *path;

	if (ap->event_sub_url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: No eventSubURL - cannot "
			   "subscribe");
		return;
	}
	if (ap->http) {
		wpa_printf(MSG_DEBUG, "WPS ER: Pending HTTP request - cannot "
			   "send subscribe request");
		return;
	}

	url = http_client_url_parse(ap->event_sub_url, &dst, &path);
	if (url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to parse eventSubURL");
		return;
	}

	req = wpabuf_alloc(os_strlen(ap->event_sub_url) + 1000);
	if (req == NULL) {
		os_free(url);
		return;
	}
	wpabuf_printf(req,
		      "SUBSCRIBE %s HTTP/1.1\r\n"
		      "HOST: %s:%d\r\n"
		      "CALLBACK: <http://%s:%d/event/%d>\r\n"
		      "NT: upnp:event\r\n"
		      "TIMEOUT: Second-%d\r\n"
		      "\r\n",
		      path, inet_ntoa(dst.sin_addr), ntohs(dst.sin_port),
		      ap->er->ip_addr_text, ap->er->http_port, ap->id, 1800);
	os_free(url);
	wpa_hexdump_ascii(MSG_MSGDUMP, "WPS ER: Subscription request",
			  wpabuf_head(req), wpabuf_len(req));

	ap->http = http_client_addr(&dst, req, 1000, wps_er_http_subscribe_cb,
				    ap);
	if (ap->http == NULL)
		wpabuf_free(req);
}


static void wps_er_parse_device_description(struct wps_er_ap *ap,
					    struct wpabuf *reply)
{
	/* Note: reply includes null termination after the buffer data */
	const char *data = wpabuf_head(reply);

	wpa_hexdump_ascii(MSG_MSGDUMP, "WPS ER: Device info",
			  wpabuf_head(reply), wpabuf_len(reply));

	ap->friendly_name = xml_get_first_item(data, "friendlyName");
	wpa_printf(MSG_DEBUG, "WPS ER: friendlyName='%s'", ap->friendly_name);

	ap->manufacturer = xml_get_first_item(data, "manufacturer");
	wpa_printf(MSG_DEBUG, "WPS ER: manufacturer='%s'", ap->manufacturer);

	ap->manufacturer_url = xml_get_first_item(data, "manufacturerURL");
	wpa_printf(MSG_DEBUG, "WPS ER: manufacturerURL='%s'",
		   ap->manufacturer_url);

	ap->model_description = xml_get_first_item(data, "modelDescription");
	wpa_printf(MSG_DEBUG, "WPS ER: modelDescription='%s'",
		   ap->model_description);

	ap->model_name = xml_get_first_item(data, "modelName");
	wpa_printf(MSG_DEBUG, "WPS ER: modelName='%s'", ap->model_name);

	ap->model_number = xml_get_first_item(data, "modelNumber");
	wpa_printf(MSG_DEBUG, "WPS ER: modelNumber='%s'", ap->model_number);

	ap->model_url = xml_get_first_item(data, "modelURL");
	wpa_printf(MSG_DEBUG, "WPS ER: modelURL='%s'", ap->model_url);

	ap->serial_number = xml_get_first_item(data, "serialNumber");
	wpa_printf(MSG_DEBUG, "WPS ER: serialNumber='%s'", ap->serial_number);

	ap->udn = xml_get_first_item(data, "UDN");
	wpa_printf(MSG_DEBUG, "WPS ER: UDN='%s'", ap->udn);

	ap->upc = xml_get_first_item(data, "UPC");
	wpa_printf(MSG_DEBUG, "WPS ER: UPC='%s'", ap->upc);

	ap->scpd_url = http_link_update(
		xml_get_first_item(data, "SCPDURL"), ap->location);
	wpa_printf(MSG_DEBUG, "WPS ER: SCPDURL='%s'", ap->scpd_url);

	ap->control_url = http_link_update(
		xml_get_first_item(data, "controlURL"), ap->location);
	wpa_printf(MSG_DEBUG, "WPS ER: controlURL='%s'", ap->control_url);

	ap->event_sub_url = http_link_update(
		xml_get_first_item(data, "eventSubURL"), ap->location);
	wpa_printf(MSG_DEBUG, "WPS ER: eventSubURL='%s'", ap->event_sub_url);
}


static void wps_er_http_dev_desc_cb(void *ctx, struct http_client *c,
				    enum http_client_event event)
{
	struct wps_er_ap *ap = ctx;
	struct wpabuf *reply;
	int subscribe = 0;

	switch (event) {
	case HTTP_CLIENT_OK:
		reply = http_client_get_body(c);
		if (reply == NULL)
			break;
		wps_er_parse_device_description(ap, reply);
		subscribe = 1;
		break;
	case HTTP_CLIENT_FAILED:
	case HTTP_CLIENT_INVALID_REPLY:
	case HTTP_CLIENT_TIMEOUT:
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to fetch device info");
		break;
	}
	http_client_free(ap->http);
	ap->http = NULL;
	if (subscribe)
		wps_er_subscribe(ap);
}


static void wps_er_ap_add(struct wps_er *er, struct in_addr *addr,
			  const char *location, int max_age)
{
	struct wps_er_ap *ap;

	ap = wps_er_ap_get(er, addr);
	if (ap) {
		/* Update advertisement timeout */
		eloop_cancel_timeout(wps_er_ap_timeout, er, ap);
		eloop_register_timeout(max_age, 0, wps_er_ap_timeout, er, ap);
		return;
	}

	ap = os_zalloc(sizeof(*ap));
	if (ap == NULL)
		return;
	ap->er = er;
	ap->id = ++er->next_ap_id;
	ap->location = os_strdup(location);
	if (ap->location == NULL) {
		os_free(ap);
		return;
	}
	ap->next = er->ap;
	er->ap = ap;

	ap->addr.s_addr = addr->s_addr;
	eloop_register_timeout(max_age, 0, wps_er_ap_timeout, er, ap);

	wpa_printf(MSG_DEBUG, "WPS ER: Added AP entry for %s (%s)",
		   inet_ntoa(ap->addr), ap->location);

	/* Fetch device description */
	ap->http = http_client_url(ap->location, NULL, 10000,
				   wps_er_http_dev_desc_cb, ap);
}


static void wps_er_ap_remove(struct wps_er *er, struct in_addr *addr)
{
	struct wps_er_ap *prev = NULL, *ap = er->ap;

	while (ap) {
		if (ap->addr.s_addr == addr->s_addr) {
			if (prev)
				prev->next = ap->next;
			else
				er->ap = ap->next;
			wps_er_ap_free(er, ap);
			return;
		}
		prev = ap;
		ap = ap->next;
	}
}


static void wps_er_ap_remove_all(struct wps_er *er)
{
	struct wps_er_ap *prev, *ap;

	ap = er->ap;
	er->ap = NULL;

	while (ap) {
		prev = ap;
		ap = ap->next;
		wps_er_ap_free(er, prev);
	}
}


static void wps_er_ssdp_rx(int sd, void *eloop_ctx, void *sock_ctx)
{
	struct wps_er *er = eloop_ctx;
	struct sockaddr_in addr; /* client address */
	socklen_t addr_len;
	int nread;
	char buf[MULTICAST_MAX_READ], *pos, *pos2, *start;
	int wfa = 0, byebye = 0;
	int max_age = -1;
	char *location = NULL;

	addr_len = sizeof(addr);
	nread = recvfrom(sd, buf, sizeof(buf) - 1, 0,
			 (struct sockaddr *) &addr, &addr_len);
	if (nread <= 0)
		return;
	buf[nread] = '\0';

	wpa_printf(MSG_DEBUG, "WPS ER: Received SSDP from %s",
		   inet_ntoa(addr.sin_addr));
	wpa_hexdump_ascii(MSG_MSGDUMP, "WPS ER: Received SSDP contents",
			  (u8 *) buf, nread);

	if (sd == er->multicast_sd) {
		/* Reply to M-SEARCH */
		if (os_strncmp(buf, "HTTP/1.1 200 OK", 15) != 0)
			return; /* unexpected response header */
	} else {
		/* Unsolicited message (likely NOTIFY or M-SEARCH) */
		if (os_strncmp(buf, "NOTIFY ", 7) != 0)
			return; /* only process notifications */
	}

	for (start = buf; start && *start; start = pos) {
		pos = os_strchr(start, '\n');
		if (pos) {
			if (pos[-1] == '\r')
				pos[-1] = '\0';
			*pos++ = '\0';
		}
		if (os_strstr(start, "schemas-wifialliance-org:device:"
			      "WFADevice:1"))
			wfa = 1;
		if (os_strstr(start, "schemas-wifialliance-org:service:"
			      "WFAWLANConfig:1"))
			wfa = 1;
		if (os_strncasecmp(start, "LOCATION:", 9) == 0) {
			start += 9;
			while (*start == ' ')
				start++;
			location = start;
		} else if (os_strncasecmp(start, "NTS:", 4) == 0) {
			if (os_strstr(start, "ssdp:byebye"))
				byebye = 1;
		} else if (os_strncasecmp(start, "CACHE-CONTROL:", 14) == 0) {
			start += 9;
			while (*start == ' ')
				start++;
			pos2 = os_strstr(start, "max-age=");
			if (pos2 == NULL)
				continue;
			pos2 += 8;
			max_age = atoi(pos2);
		}
	}

	if (!wfa)
		return; /* Not WPS advertisement/reply */

	if (byebye) {
		wps_er_ap_remove(er, &addr.sin_addr);
		return;
	}

	if (!location)
		return; /* Unknown location */

	if (max_age < 1)
		return; /* No max-age reported */

	wpa_printf(MSG_DEBUG, "WPS ER: AP discovered: %s "
		   "(packet source: %s  max-age: %d)",
		   location, inet_ntoa(addr.sin_addr), max_age);

	wps_er_ap_add(er, &addr.sin_addr, location, max_age);
}


static void wps_er_send_ssdp_msearch(struct wps_er *er)
{
	struct wpabuf *msg;
	struct sockaddr_in dest;

	msg = wpabuf_alloc(500);
	if (msg == NULL)
		return;

	wpabuf_put_str(msg,
		       "M-SEARCH * HTTP/1.1\r\n"
		       "HOST: 239.255.255.250:1900\r\n"
		       "MAN: \"ssdp:discover\"\r\n"
		       "MX: 3\r\n"
		       "ST: urn:schemas-wifialliance-org:device:WFADevice:1"
		       "\r\n"
		       "\r\n");

	os_memset(&dest, 0, sizeof(dest));
	dest.sin_family = AF_INET;
	dest.sin_addr.s_addr = inet_addr(UPNP_MULTICAST_ADDRESS);
	dest.sin_port = htons(UPNP_MULTICAST_PORT);

	if (sendto(er->multicast_sd, wpabuf_head(msg), wpabuf_len(msg), 0,
		   (struct sockaddr *) &dest, sizeof(dest)) < 0)
		wpa_printf(MSG_DEBUG, "WPS ER: M-SEARCH sendto failed: "
			   "%d (%s)", errno, strerror(errno));

	wpabuf_free(msg);
}


static void http_put_date(struct wpabuf *buf)
{
	wpabuf_put_str(buf, "Date: ");
	format_date(buf);
	wpabuf_put_str(buf, "\r\n");
}


static void wps_er_http_resp_not_found(struct http_request *req)
{
	struct wpabuf *buf;
	buf = wpabuf_alloc(200);
	if (buf == NULL) {
		http_request_deinit(req);
		return;
	}

	wpabuf_put_str(buf,
		       "HTTP/1.1 404 Not Found\r\n"
		       "Server: unspecified, UPnP/1.0, unspecified\r\n"
		       "Connection: close\r\n");
	http_put_date(buf);
	wpabuf_put_str(buf, "\r\n");
	http_request_send_and_deinit(req, buf);
}


static void wps_er_http_resp_ok(struct http_request *req)
{
	struct wpabuf *buf;
	buf = wpabuf_alloc(200);
	if (buf == NULL) {
		http_request_deinit(req);
		return;
	}

	wpabuf_put_str(buf,
		       "HTTP/1.1 200 OK\r\n"
		       "Server: unspecified, UPnP/1.0, unspecified\r\n"
		       "Connection: close\r\n"
		       "Content-Length: 0\r\n");
	http_put_date(buf);
	wpabuf_put_str(buf, "\r\n");
	http_request_send_and_deinit(req, buf);
}


static void wps_er_sta_timeout(void *eloop_data, void *user_ctx)
{
	struct wps_er_sta *sta = eloop_data;
	wpa_printf(MSG_DEBUG, "WPS ER: STA entry timed out");
	wps_er_sta_free(sta);
}


static struct wps_er_sta * wps_er_add_sta_data(struct wps_er_ap *ap,
					       const u8 *addr,
					       struct wps_parse_attr *attr,
					       int probe_req)
{
	struct wps_er_sta *sta = wps_er_sta_get(ap, addr);

	if (sta == NULL) {
		sta = os_zalloc(sizeof(*sta));
		if (sta == NULL)
			return NULL;
		os_memcpy(sta->addr, addr, ETH_ALEN);
		sta->ap = ap;
		sta->next = ap->sta;
		ap->sta = sta;
	}

	if (!probe_req)
		sta->m1_received = 1;

	if (attr->config_methods && (!probe_req || !sta->m1_received))
		sta->config_methods = WPA_GET_BE16(attr->config_methods);
	if (attr->uuid_e && (!probe_req || !sta->m1_received))
		os_memcpy(sta->uuid, attr->uuid_e, WPS_UUID_LEN);
	if (attr->primary_dev_type && (!probe_req || !sta->m1_received))
		os_memcpy(sta->pri_dev_type, attr->primary_dev_type, 8);
	if (attr->dev_password_id && (!probe_req || !sta->m1_received))
		sta->dev_passwd_id = WPA_GET_BE16(attr->dev_password_id);

	if (attr->manufacturer) {
		os_free(sta->manufacturer);
		sta->manufacturer = os_malloc(attr->manufacturer_len + 1);
		if (sta->manufacturer) {
			os_memcpy(sta->manufacturer, attr->manufacturer,
				  attr->manufacturer_len);
			sta->manufacturer[attr->manufacturer_len] = '\0';
		}
	}

	if (attr->model_name) {
		os_free(sta->model_name);
		sta->model_name = os_malloc(attr->model_name_len + 1);
		if (sta->model_name) {
			os_memcpy(sta->model_name, attr->model_name,
				  attr->model_name_len);
			sta->model_name[attr->model_name_len] = '\0';
		}
	}

	if (attr->model_number) {
		os_free(sta->model_number);
		sta->model_number = os_malloc(attr->model_number_len + 1);
		if (sta->model_number) {
			os_memcpy(sta->model_number, attr->model_number,
				  attr->model_number_len);
			sta->model_number[attr->model_number_len] = '\0';
		}
	}

	if (attr->serial_number) {
		os_free(sta->serial_number);
		sta->serial_number = os_malloc(attr->serial_number_len + 1);
		if (sta->serial_number) {
			os_memcpy(sta->serial_number, attr->serial_number,
				  attr->serial_number_len);
			sta->serial_number[attr->serial_number_len] = '\0';
		}
	}

	if (attr->dev_name) {
		os_free(sta->dev_name);
		sta->dev_name = os_malloc(attr->dev_name_len + 1);
		if (sta->dev_name) {
			os_memcpy(sta->dev_name, attr->dev_name,
				  attr->dev_name_len);
			sta->dev_name[attr->dev_name_len] = '\0';
		}
	}

	eloop_cancel_timeout(wps_er_sta_timeout, sta, NULL);
	eloop_register_timeout(300, 0, wps_er_sta_timeout, sta, NULL);

	/* TODO: wpa_msg indication if new STA */

	return sta;
}


static void wps_er_process_wlanevent_probe_req(struct wps_er_ap *ap,
					       const u8 *addr,
					       struct wpabuf *msg)
{
	struct wps_parse_attr attr;

	wpa_printf(MSG_DEBUG, "WPS ER: WLANEvent - Probe Request - from "
		   MACSTR, MAC2STR(addr));
	wpa_hexdump_buf(MSG_MSGDUMP, "WPS ER: WLANEvent - Enrollee's message "
			"(TLVs from Probe Request)", msg);

	if (wps_parse_msg(msg, &attr) < 0) {
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to parse TLVs in "
			   "WLANEvent message");
		return;
	}

	wps_er_add_sta_data(ap, addr, &attr, 1);
}


static void wps_er_http_put_wlan_response_cb(void *ctx, struct http_client *c,
					     enum http_client_event event)
{
	struct wps_er_sta *sta = ctx;

	switch (event) {
	case HTTP_CLIENT_OK:
		wpa_printf(MSG_DEBUG, "WPS ER: PutWLANResponse OK");
		break;
	case HTTP_CLIENT_FAILED:
	case HTTP_CLIENT_INVALID_REPLY:
	case HTTP_CLIENT_TIMEOUT:
		wpa_printf(MSG_DEBUG, "WPS ER: PutWLANResponse failed");
		break;
	}
	http_client_free(sta->http);
	sta->http = NULL;
}


static const char *soap_prefix =
	"<?xml version=\"1.0\"?>\n"
	"<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
	"s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
	"<s:Body>\n";
static const char *soap_postfix =
	"</s:Body>\n</s:Envelope>\n";
static const char *urn_wfawlanconfig =
	"urn:schemas-wifialliance-org:service:WFAWLANConfig:1";

static struct wpabuf * wps_er_soap_hdr(const struct wpabuf *msg,
				       const char *name, const char *path,
				       const struct sockaddr_in *dst,
				       char **len_ptr, char **body_ptr)
{
	unsigned char *encoded;
	size_t encoded_len;
	struct wpabuf *buf;

	encoded = base64_encode(wpabuf_head(msg), wpabuf_len(msg),
				&encoded_len);
	if (encoded == NULL)
		return NULL;

	buf = wpabuf_alloc(1000 + encoded_len);
	if (buf == NULL) {
		os_free(encoded);
		return NULL;
	}

	wpabuf_printf(buf,
		      "POST %s HTTP/1.1\r\n"
		      "Host: %s:%d\r\n"
		      "Content-Type: text/xml; charset=\"utf-8\"\r\n"
		      "Content-Length: ",
		      path, inet_ntoa(dst->sin_addr), ntohs(dst->sin_port));

	*len_ptr = wpabuf_put(buf, 0);
	wpabuf_printf(buf,
		      "        \r\n"
		      "SOAPACTION: \"%s#%s\"\r\n"
		      "\r\n",
		      urn_wfawlanconfig, name);

	*body_ptr = wpabuf_put(buf, 0);

	wpabuf_put_str(buf, soap_prefix);
	wpabuf_printf(buf, "<u:%s xmlns:u=\"", name);
	wpabuf_put_str(buf, urn_wfawlanconfig);
	wpabuf_put_str(buf, "\">\n");
	wpabuf_printf(buf, "<NewMessage>%s</NewMessage>\n", (char *) encoded);
	os_free(encoded);

	return buf;
}


static void wps_er_soap_end(struct wpabuf *buf, const char *name,
			    char *len_ptr, char *body_ptr)
{
	char len_buf[10];
	wpabuf_printf(buf, "</u:%s>\n", name);
	wpabuf_put_str(buf, soap_postfix);
	os_snprintf(len_buf, sizeof(len_buf), "%d",
		    (int) ((char *) wpabuf_put(buf, 0) - body_ptr));
	os_memcpy(len_ptr, len_buf, os_strlen(len_buf));
}


static void wps_er_sta_send_msg(struct wps_er_sta *sta, struct wpabuf *msg)
{
	struct wpabuf *buf;
	char *len_ptr, *body_ptr;
	struct sockaddr_in dst;
	char *url, *path;

	if (sta->http) {
		wpa_printf(MSG_DEBUG, "WPS ER: Pending HTTP request for STA - "
			   "ignore new request");
		wpabuf_free(msg);
		return;
	}

	if (sta->ap->control_url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: No controlURL for AP");
		wpabuf_free(msg);
		return;
	}

	url = http_client_url_parse(sta->ap->control_url, &dst, &path);
	if (url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to parse controlURL");
		wpabuf_free(msg);
		return;
	}

	buf = wps_er_soap_hdr(msg, "PutWLANResponse", path, &dst, &len_ptr,
			      &body_ptr);
	wpabuf_free(msg);
	os_free(url);
	if (buf == NULL)
		return;
	wpabuf_printf(buf, "<NewWLANEventType>%d</NewWLANEventType>\n",
		      UPNP_WPS_WLANEVENT_TYPE_EAP);
	wpabuf_printf(buf, "<NewWLANEventMAC>" MACSTR "</NewWLANEventMAC>\n",
		      MAC2STR(sta->addr));

	wps_er_soap_end(buf, "PutWLANResponse", len_ptr, body_ptr);

	sta->http = http_client_addr(&dst, buf, 1000,
				     wps_er_http_put_wlan_response_cb, sta);
	if (sta->http == NULL)
		wpabuf_free(buf);
}


static void wps_er_sta_process(struct wps_er_sta *sta, struct wpabuf *msg,
			       enum wsc_op_code op_code)
{
	enum wps_process_res res;

	res = wps_process_msg(sta->wps, op_code, msg);
	if (res == WPS_CONTINUE) {
		struct wpabuf *next = wps_get_msg(sta->wps, &op_code);
		if (next)
			wps_er_sta_send_msg(sta, next);
	}
}


static void wps_er_sta_start(struct wps_er_sta *sta, struct wpabuf *msg)
{
	struct wps_config cfg;

	if (sta->wps)
		wps_deinit(sta->wps);

	os_memset(&cfg, 0, sizeof(cfg));
	cfg.wps = sta->ap->er->wps;
	cfg.registrar = 1;
	cfg.peer_addr = sta->addr;

	sta->wps = wps_init(&cfg);
	if (sta->wps == NULL)
		return;

	wps_er_sta_process(sta, msg, WSC_MSG);
}


static void wps_er_process_wlanevent_eap(struct wps_er_ap *ap, const u8 *addr,
					 struct wpabuf *msg)
{
	struct wps_parse_attr attr;
	struct wps_er_sta *sta;

	wpa_printf(MSG_DEBUG, "WPS ER: WLANEvent - EAP - from " MACSTR,
		   MAC2STR(addr));
	wpa_hexdump_buf(MSG_MSGDUMP, "WPS ER: WLANEvent - Enrollee's message "
			"(TLVs from EAP-WSC)", msg);

	if (wps_parse_msg(msg, &attr) < 0) {
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to parse TLVs in "
			   "WLANEvent message");
		return;
	}

	sta = wps_er_add_sta_data(ap, addr, &attr, 0);

	if (attr.msg_type && *attr.msg_type == WPS_M1)
		wps_er_sta_start(sta, msg);
	else if (sta->wps) {
		enum wsc_op_code op_code = WSC_MSG;
		if (attr.msg_type) {
			switch (*attr.msg_type) {
			case WPS_WSC_ACK:
				op_code = WSC_ACK;
				break;
			case WPS_WSC_NACK:
				op_code = WSC_NACK;
				break;
			case WPS_WSC_DONE:
				op_code = WSC_Done;
				break;
			}
		}
		wps_er_sta_process(sta, msg, op_code);
	}
}


static void wps_er_process_wlanevent(struct wps_er_ap *ap,
				     struct wpabuf *event)
{
	u8 *data;
	u8 wlan_event_type;
	u8 wlan_event_mac[ETH_ALEN];
	struct wpabuf msg;

	wpa_hexdump(MSG_MSGDUMP, "WPS ER: Received WLANEvent",
		    wpabuf_head(event), wpabuf_len(event));
	if (wpabuf_len(event) < 1 + 17) {
		wpa_printf(MSG_DEBUG, "WPS ER: Too short WLANEvent");
		return;
	}

	data = wpabuf_mhead(event);
	wlan_event_type = data[0];
	if (hwaddr_aton((char *) data + 1, wlan_event_mac) < 0) {
		wpa_printf(MSG_DEBUG, "WPS ER: Invalid WLANEventMAC in "
			   "WLANEvent");
		return;
	}

	wpabuf_set(&msg, data + 1 + 17, wpabuf_len(event) - (1 + 17));

	switch (wlan_event_type) {
	case 1:
		wps_er_process_wlanevent_probe_req(ap, wlan_event_mac, &msg);
		break;
	case 2:
		wps_er_process_wlanevent_eap(ap, wlan_event_mac, &msg);
		break;
	default:
		wpa_printf(MSG_DEBUG, "WPS ER: Unknown WLANEventType %d",
			   wlan_event_type);
		break;
	}
}


static void wps_er_http_event(struct wps_er *er, struct http_request *req,
			      unsigned int ap_id)
{
	struct wps_er_ap *ap = wps_er_ap_get_id(er, ap_id);
	struct wpabuf *event;
	enum http_reply_code ret;

	if (ap == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: HTTP event from unknown AP id "
			   "%u", ap_id);
		wps_er_http_resp_not_found(req);
		return;
	}
	wpa_printf(MSG_MSGDUMP, "WPS ER: HTTP event from AP id %u: %s",
		   ap_id, http_request_get_data(req));

	event = xml_get_base64_item(http_request_get_data(req), "WLANEvent",
				    &ret);
	if (event == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: Could not extract WLANEvent "
			   "from the event notification");
		/*
		 * Reply with OK anyway to avoid getting unregistered from
		 * events.
		 */
		wps_er_http_resp_ok(req);
		return;
	}

	wps_er_process_wlanevent(ap, event);

	wpabuf_free(event);
	wps_er_http_resp_ok(req);
}


static void wps_er_http_notify(struct wps_er *er, struct http_request *req)
{
	char *uri = http_request_get_uri(req);

	if (os_strncmp(uri, "/event/", 7) == 0) {
		wps_er_http_event(er, req, atoi(uri + 7));
	} else {
		wpa_printf(MSG_DEBUG, "WPS ER: Unknown HTTP NOTIFY for '%s'",
			   uri);
		wps_er_http_resp_not_found(req);
	}
}


static void wps_er_http_req(void *ctx, struct http_request *req)
{
	struct wps_er *er = ctx;
	struct sockaddr_in *cli = http_request_get_cli_addr(req);
	enum httpread_hdr_type type = http_request_get_type(req);
	struct wpabuf *buf;

	wpa_printf(MSG_DEBUG, "WPS ER: HTTP request: '%s' (type %d) from "
		   "%s:%d",
		   http_request_get_uri(req), type,
		   inet_ntoa(cli->sin_addr), ntohs(cli->sin_port));

	switch (type) {
	case HTTPREAD_HDR_TYPE_NOTIFY:
		wps_er_http_notify(er, req);
		break;
	default:
		wpa_printf(MSG_DEBUG, "WPS ER: Unsupported HTTP request type "
			   "%d", type);
		buf = wpabuf_alloc(200);
		if (buf == NULL) {
			http_request_deinit(req);
			return;
		}
		wpabuf_put_str(buf,
			       "HTTP/1.1 501 Unimplemented\r\n"
			       "Connection: close\r\n");
		http_put_date(buf);
		wpabuf_put_str(buf, "\r\n");
		http_request_send_and_deinit(req, buf);
		break;
	}
}


struct wps_er *
wps_er_init(struct wps_context *wps, const char *ifname)
{
	struct wps_er *er;
	struct in_addr addr;

	er = os_zalloc(sizeof(*er));
	if (er == NULL)
		return NULL;

	er->multicast_sd = -1;
	er->ssdp_sd = -1;

	os_strlcpy(er->ifname, ifname, sizeof(er->ifname));
	er->wps = wps;

	if (get_netif_info(ifname,
			   &er->ip_addr, &er->ip_addr_text,
			   er->mac_addr, &er->mac_addr_text)) {
		wpa_printf(MSG_INFO, "WPS UPnP: Could not get IP/MAC address "
			   "for %s. Does it have IP address?", ifname);
		wps_er_deinit(er);
		return NULL;
	}

	if (add_ssdp_network(ifname)) {
		wps_er_deinit(er);
		return NULL;
	}

	er->multicast_sd = ssdp_open_multicast_sock(er->ip_addr);
	if (er->multicast_sd < 0) {
		wps_er_deinit(er);
		return NULL;
	}

	er->ssdp_sd = ssdp_listener_open();
	if (er->ssdp_sd < 0) {
		wps_er_deinit(er);
		return NULL;
	}
	if (eloop_register_sock(er->multicast_sd, EVENT_TYPE_READ,
				wps_er_ssdp_rx, er, NULL) ||
	    eloop_register_sock(er->ssdp_sd, EVENT_TYPE_READ,
				wps_er_ssdp_rx, er, NULL)) {
		wps_er_deinit(er);
		return NULL;
	}

	addr.s_addr = er->ip_addr;
	er->http_srv = http_server_init(&addr, -1, wps_er_http_req, er);
	if (er->http_srv == NULL) {
		wps_er_deinit(er);
		return NULL;
	}
	er->http_port = http_server_get_port(er->http_srv);

	wpa_printf(MSG_DEBUG, "WPS ER: Start (ifname=%s ip_addr=%s "
		   "mac_addr=%s)",
		   er->ifname, er->ip_addr_text, er->mac_addr_text);

	wps_er_send_ssdp_msearch(er);

	return er;
}


void wps_er_deinit(struct wps_er *er)
{
	if (er == NULL)
		return;
	http_server_deinit(er->http_srv);
	wps_er_ap_remove_all(er);
	if (er->multicast_sd >= 0) {
		eloop_unregister_sock(er->multicast_sd, EVENT_TYPE_READ);
		close(er->multicast_sd);
	}
	if (er->ssdp_sd >= 0) {
		eloop_unregister_sock(er->ssdp_sd, EVENT_TYPE_READ);
		close(er->ssdp_sd);
	}
	os_free(er->ip_addr_text);
	os_free(er->mac_addr_text);
	os_free(er);
}


static void wps_er_http_set_sel_reg_cb(void *ctx, struct http_client *c,
				       enum http_client_event event)
{
	struct wps_er_ap *ap = ctx;

	switch (event) {
	case HTTP_CLIENT_OK:
		wpa_printf(MSG_DEBUG, "WPS ER: SetSelectedRegistrar OK");
		break;
	case HTTP_CLIENT_FAILED:
	case HTTP_CLIENT_INVALID_REPLY:
	case HTTP_CLIENT_TIMEOUT:
		wpa_printf(MSG_DEBUG, "WPS ER: SetSelectedRegistrar failed");
		break;
	}
	http_client_free(ap->http);
	ap->http = NULL;
}


static void wps_er_send_set_sel_reg(struct wps_er_ap *ap, struct wpabuf *msg)
{
	struct wpabuf *buf;
	char *len_ptr, *body_ptr;
	struct sockaddr_in dst;
	char *url, *path;

	if (ap->control_url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: No controlURL for AP");
		return;
	}

	if (ap->http) {
		wpa_printf(MSG_DEBUG, "WPS ER: Pending HTTP request for AP - "
			   "ignore new request");
		return;
	}

	url = http_client_url_parse(ap->control_url, &dst, &path);
	if (url == NULL) {
		wpa_printf(MSG_DEBUG, "WPS ER: Failed to parse controlURL");
		return;
	}

	buf = wps_er_soap_hdr(msg, "SetSelectedRegistrar", path, &dst,
			      &len_ptr, &body_ptr);
	os_free(url);
	if (buf == NULL)
		return;

	wps_er_soap_end(buf, "SetSelectedRegistrar", len_ptr, body_ptr);

	ap->http = http_client_addr(&dst, buf, 1000,
				    wps_er_http_set_sel_reg_cb, ap);
	if (ap->http == NULL)
		wpabuf_free(buf);
}


static int wps_er_build_selected_registrar(struct wpabuf *msg, int sel_reg)
{
	wpabuf_put_be16(msg, ATTR_SELECTED_REGISTRAR);
	wpabuf_put_be16(msg, 1);
	wpabuf_put_u8(msg, !!sel_reg);
	return 0;
}


static int wps_er_build_dev_password_id(struct wpabuf *msg, u16 dev_passwd_id)
{
	wpabuf_put_be16(msg, ATTR_DEV_PASSWORD_ID);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, dev_passwd_id);
	return 0;
}


static int wps_er_build_sel_reg_config_methods(struct wpabuf *msg,
					       u16 sel_reg_config_methods)
{
	wpabuf_put_be16(msg, ATTR_SELECTED_REGISTRAR_CONFIG_METHODS);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, sel_reg_config_methods);
	return 0;
}


void wps_er_set_sel_reg(struct wps_er *er, int sel_reg, u16 dev_passwd_id,
			u16 sel_reg_config_methods)
{
	struct wpabuf *msg;
	struct wps_er_ap *ap;

	msg = wpabuf_alloc(500);
	if (msg == NULL)
		return;

	if (wps_build_version(msg) ||
	    wps_er_build_selected_registrar(msg, sel_reg) ||
	    wps_er_build_dev_password_id(msg, dev_passwd_id) ||
	    wps_er_build_sel_reg_config_methods(msg, sel_reg_config_methods)) {
		wpabuf_free(msg);
		return;
	}

	for (ap = er->ap; ap; ap = ap->next)
		wps_er_send_set_sel_reg(ap, msg);

	wpabuf_free(msg);
}