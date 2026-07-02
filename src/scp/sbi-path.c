/*
 * Copyright (C) 2019-2024 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sbi-path.h"

static int request_handler(ogs_sbi_request_t *request, void *data);
static int response_handler(
        int status, ogs_sbi_response_t *response, void *data);
static int nf_discover_handler(
        int status, ogs_sbi_response_t *response, void *data);
static int sepp_discover_handler(
        int status, ogs_sbi_response_t *response, void *data);

static bool send_discover(
        ogs_sbi_client_t *client, ogs_sbi_client_cb_f client_cb,
        scp_assoc_t *assoc);

static bool send_request(
        ogs_sbi_client_t *client, ogs_sbi_client_cb_f client_cb,
        ogs_sbi_request_t *request, bool do_not_remove_custom_header,
        scp_assoc_t *assoc);

static void copy_request(
        ogs_sbi_request_t *target, ogs_sbi_request_t *source,
        bool do_not_remove_custom_header);

/* ===== TYcustom: UE-affinity routing for UDR (ueid % N) =====================
 *
 * When multiple UDR instances are deployed, route every SBI request for a given
 * UE to ONE fixed UDR, keyed by idx = imsi % N. This keeps a UE's entire
 * registration AND deregistration served by a single UDR. Routing depends only
 * on the imsi carried in the request URI, so it is identical for register and
 * dereg, and for both callers (UDM->UDR and PCF->UDR).
 *
 * See UDR_SCALEOUT_SCP_ROUTING_PLAN.md for the full design and rationale.
 * ------------------------------------------------------------------------- */

/* Max UDR candidates collected per request (>> any realistic replica count). */
#define SCP_MAX_UDR 64

/* Extract the UE's imsi digit string from the request URI. Covers both UDR
 * path shapes:
 *   /nudr-dr/v1/subscription-data/imsi-<digits>/...   (UDM: register + dereg)
 *   /nudr-dr/v1/policy-data/ues/imsi-<digits>/...      (PCF)
 * Grabs the digits right after the first "imsi-". Returns false if none. */
static bool scp_udr_extract_imsi_digits(
        const char *uri, char *buf, size_t buflen)
{
    const char *p;
    size_t n = 0;

    if (!uri) return false;
    p = strstr(uri, "imsi-");
    if (!p) return false;
    p += 5; /* skip "imsi-" */
    while (*p >= '0' && *p <= '9' && n + 1 < buflen)
        buf[n++] = *p++;
    if (n == 0) return false;
    buf[n] = '\0';
    return true;
}

/* Stable sort key for a UDR instance = its pod IP string. In this deployment
 * the UDR config uses `sbi.server: - dev: eth0` (no advertise), so each UDR
 * registers to NRF with its own pod IP and nf_instance->fqdn is empty. Pod IP
 * is constant for the life of the experiment (pods are not restarted), so the
 * ordering is stable and reproducible. Read-only; do not free. Note
 * ogs_sockaddr_to_string_static() returns a shared static buffer. */
static const char *scp_udr_sort_key(ogs_sbi_nf_instance_t *nf)
{
    if (nf->num_of_ipv4 > 0 && nf->ipv4[0])
        return ogs_sockaddr_to_string_static(nf->ipv4[0]);
    if (nf->fqdn)
        return nf->fqdn;
    return nf->id;
}

/* Collect every "target==UDR and discovery-matched" instance from the global
 * nf_instance_list, insertion-sort them ascending by pod IP, return the count;
 * result written into out[] (capacity out_max). */
static int scp_collect_sorted_udr(
        OpenAPI_nf_type_e target_nf_type,
        OpenAPI_nf_type_e requester_nf_type,
        ogs_sbi_discovery_option_t *discovery_option,
        ogs_sbi_nf_instance_t **out, int out_max)
{
    ogs_sbi_nf_instance_t *nf = NULL;
    int cnt = 0, i, j;

    ogs_list_for_each(&ogs_sbi_self()->nf_instance_list, nf) {
        if (!ogs_sbi_discovery_param_is_matched(
                nf, target_nf_type, requester_nf_type, discovery_option))
            continue;
        if (cnt < out_max)
            out[cnt++] = nf;
    }

    /* Insertion sort (N is small). ogs_sockaddr_to_string_static() shares one
     * static buffer, so copy the inserted element's key onto the stack (kk[])
     * before comparing, to avoid two calls overwriting each other. */
    for (i = 1; i < cnt; i++) {
        ogs_sbi_nf_instance_t *key = out[i];
        char kk[OGS_ADDRSTRLEN];
        ogs_cpystrn(kk, scp_udr_sort_key(key), sizeof(kk));
        j = i - 1;
        while (j >= 0) {
            if (strcmp(scp_udr_sort_key(out[j]), kk) <= 0) break;
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return cnt;
}

int scp_sbi_open(void)
{
    ogs_sbi_nf_instance_t *nf_instance = NULL, *nrf_instance = NULL;
    ogs_sbi_client_t *nrf_client = NULL, *next_scp = NULL;

    /* Initialize SELF NF instance */
    nf_instance = ogs_sbi_self()->nf_instance;
    ogs_assert(nf_instance);
    ogs_sbi_nf_fsm_init(nf_instance);

    /* Build NF instance information. It will be transmitted to NRF. */
    ogs_sbi_nf_instance_build_default(nf_instance);

    /*
     * If the SCP is running in Model D,
     * it can send NFRegister/NFStatusSubscribe messages to the NRF.
     */
    nrf_instance = ogs_sbi_self()->nrf_instance;
    nrf_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->nrf_instance);

    if (nrf_client) {

        /* Initialize NRF NF Instance */
        if (nrf_instance)
            ogs_sbi_nf_fsm_init(nrf_instance);
    }

    /* Check if Next-SCP's client */
    if (ogs_sbi_self()->client_delegated_config.scp.next ==
            OGS_SBI_CLIENT_DELEGATED_AUTO) {
        next_scp = NF_INSTANCE_CLIENT(ogs_sbi_self()->scp_instance);
    } else if (ogs_sbi_self()->client_delegated_config.scp.next ==
            OGS_SBI_CLIENT_DELEGATED_YES) {
        next_scp = NF_INSTANCE_CLIENT(ogs_sbi_self()->scp_instance);
        ogs_assert(next_scp);
    }

    /* If the SCP has an NRF client and does not delegate to Next-SCP */
    if (nrf_client && !next_scp) {

        /* Setup Subscription-Data */
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_SEPP, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_AMF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_AUSF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_BSF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_NSSF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_PCF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_SMF, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_UDM, NULL);
        ogs_sbi_subscription_spec_add(OpenAPI_nf_type_UDR, NULL);
    }

    if (ogs_sbi_server_start_all(request_handler) != OGS_OK)
        return OGS_ERROR;

    return OGS_OK;
}

void scp_sbi_close(void)
{
    ogs_sbi_client_stop_all();
    ogs_sbi_server_stop_all();
}

static int request_handler(ogs_sbi_request_t *request, void *data)
{
    int rv;
    ogs_hash_index_t *hi;
    ogs_sbi_client_t *client = NULL, *nrf_client = NULL, *next_scp = NULL;
    ogs_sbi_client_t *sepp_client = NULL;
    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;

    OpenAPI_nf_type_e target_nf_type = OpenAPI_nf_type_NULL;
    OpenAPI_nf_type_e requester_nf_type = OpenAPI_nf_type_NULL;
    ogs_sbi_discovery_option_t *discovery_option = NULL;
    ogs_sbi_service_type_e service_type = OGS_SBI_SERVICE_TYPE_NULL;
    bool discovery_presence = false;

    scp_assoc_t *assoc = NULL;
    ogs_sbi_nf_instance_t *nf_instance = NULL;

    struct {
        char *target_apiroot;
        char *callback;
        char *nrf_uri;
    } headers = {
        NULL, NULL, NULL
    };

    scp_event_t *e = NULL;

    ogs_assert(request);
    ogs_assert(request->h.uri);

    /* TYcustom: REQ_RX -- the SCP received a request from a consumer NF. The SCP
     * registers its own request_handler (not lib/sbi's ogs_sbi_server_handler),
     * so this point would otherwise be missed; emit it here so each SCP hop has
     * all four views. ueid is left empty (extracted offline from the URI). */
    ogs_http_log_request(OGS_HTTP_LOG_REQ_RX, request, NULL);

    stream_id = OGS_POINTER_TO_UINT(data);
    ogs_assert(stream_id >= OGS_MIN_POOL_ID &&
            stream_id <= OGS_MAX_POOL_ID);

    stream = ogs_sbi_stream_find_by_id(stream_id);
    if (!stream) {
        ogs_error("STREAM has already been removed [%d]", stream_id);
        return OGS_ERROR;
    }

    /* SCP Context */
    assoc = scp_assoc_add(stream_id);
    if (!assoc) {
        ogs_error("scp_assoc_add() failed");
        return OGS_ERROR;
    }

    /* Next-SCP client */
    if (ogs_sbi_self()->client_delegated_config.scp.next ==
            OGS_SBI_CLIENT_DELEGATED_AUTO) {
        next_scp = NF_INSTANCE_CLIENT(ogs_sbi_self()->scp_instance);
    } else if (ogs_sbi_self()->client_delegated_config.scp.next ==
            OGS_SBI_CLIENT_DELEGATED_YES) {
        next_scp = NF_INSTANCE_CLIENT(ogs_sbi_self()->scp_instance);
        ogs_assert(next_scp);
    }

    /* NRF client */
    nrf_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->nrf_instance);

    /* Discovery Option */
    discovery_option = assoc->discovery_option;
    ogs_assert(discovery_option);

    /* Extract HTTP Header */
    for (hi = ogs_hash_first(request->http.headers);
            hi; hi = ogs_hash_next(hi)) {
        char *key = (char *)ogs_hash_this_key(hi);
        char *val = ogs_hash_this_val(hi);

        if (!key || !val) {
            ogs_error("No Key[%s] Value[%s]", key, val);
            continue;
        }

        /*
         * <RFC 2616>
         *  Each header field consists of a name followed by a colon (":")
         *  and the field value. Field names are case-insensitive.
         */
        if (!strcasecmp(key, OGS_SBI_USER_AGENT)) {
            /*
             * TS29.500
             * 5.2 HTTP/2 Protocol
             * 5.2.2.2 Mandatory to support HTTP standard headers
             *
             * Table 5.2.2.2-1
             * Mandatory to support HTTP request standard headers
             *
             * Name: User-Agent
             * Reference: IETF RFC 7231 [11]
             * Description:
             * This header shall be mainly used to identify the NF type of the
             * HTTP/2 client. This header should be included in every HTTP/2
             * request sent over any SBI; This header shall be included in
             * every HTTP/2 request sent using indirect communication when
             * target NF (re-)selection is to be performed at SCP.
             *
             * For Indirect communications, the User-Agent header in a
             * request that is:
             *  - forwarded by the SCP (with or without delegated
             *    discovery) shall identify the NF type of the original NF
             *    that issued the request (i.e. the SCP shall forward the
             *    header received in the incoming request);
             *  - originated by the SCP towards the NRF (e.g. NF Discovery or
             *    Access Token Request) shall identify the SCP.
             *
             * The pattern of the content should start with the value of NF type
             * (e.g. "UDM", see NOTE 1) or "SCP" (for a request originated by
             * an SCP) and followed by a "-" and any other specific information
             * if needed afterwards.
             */
            char *v = strsep(&val, "-");
            if (v) requester_nf_type = OpenAPI_nf_type_FromString(v);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_TARGET_APIROOT)) {
            headers.target_apiroot = val;
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_CALLBACK)) {
            headers.callback = val;
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_NRF_URI)) {
            headers.nrf_uri = val;
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_TARGET_NF_TYPE)) {
            if (val) target_nf_type = OpenAPI_nf_type_FromString(val);
        } else if (!strcasecmp(key,
                    OGS_SBI_CUSTOM_DISCOVERY_REQUESTER_NF_TYPE)) {
            ogs_warn("Use User-Agent instead of Discovery-requester-nf-type");
        } else if (!strcasecmp(key,
                    OGS_SBI_CUSTOM_DISCOVERY_TARGET_NF_INSTANCE_ID)) {
            ogs_sbi_discovery_option_set_target_nf_instance_id(
                    discovery_option, val);
        } else if (!strcasecmp(key,
                    OGS_SBI_CUSTOM_DISCOVERY_REQUESTER_NF_INSTANCE_ID)) {
            ogs_sbi_discovery_option_set_requester_nf_instance_id(
                    discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_SERVICE_NAMES)) {
            if (val)
                ogs_sbi_discovery_option_parse_service_names(
                        discovery_option, val);

            /*
             * So, we'll use the first item in service-names list.
             *
             * TS29.500
             * 6.10 Support of Indirect Communication
             * 6.10.3 NF Discovery and Selection for indirect communication
             *        with Delegated Discovery
             * 6.10.3.2 Conveyance of NF Discovery Factors
             *
             * If the NF service consumer includes more than one service name
             * in the 3gpp-Sbi-Discovery-service-names header, the service name
             * corresponding to the service request shall be listed
             * as the first service name in the header.
             *
             * NOTE 3: The SCP can assume that the service request corresponds
             * to the first service name in the header.
             */
            if (discovery_option->num_of_service_names) {
                service_type = ogs_sbi_service_type_from_name(
                                    discovery_option->service_names[0]);
            }
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_SNSSAIS)) {
            if (val)
                ogs_sbi_discovery_option_parse_snssais(discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_GUAMI)) {
            if (val)
                ogs_sbi_discovery_option_parse_guami(discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_DNN)) {
            ogs_sbi_discovery_option_set_dnn(discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_TAI)) {
            if (val)
                ogs_sbi_discovery_option_parse_tai(discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_GUAMI)) {
            if (val)
                ogs_sbi_discovery_option_parse_guami(discovery_option, val);
        } else if (!strcasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_TARGET_PLMN_LIST)) {
            if (val)
                discovery_option->num_of_target_plmn_list =
                    ogs_sbi_discovery_option_parse_plmn_list(
                        discovery_option->target_plmn_list, val);
        } else if (!strcasecmp(key,
                    OGS_SBI_CUSTOM_DISCOVERY_REQUESTER_PLMN_LIST)) {
            if (val)
                discovery_option->num_of_requester_plmn_list =
                    ogs_sbi_discovery_option_parse_plmn_list(
                        discovery_option->requester_plmn_list, val);
        } else if (!strcasecmp(key,
                    OGS_SBI_CUSTOM_DISCOVERY_REQUESTER_FEATURES)) {
            if (val)
                discovery_option->requester_features =
                    ogs_uint64_from_string_hexadecimal(val);
        } else {
            /* ':scheme' and ':authority' will be automatically filled in later */
        }
    }

    /* Check if Discovery Parameter and Option */
    discovery_presence = false;

    if (!requester_nf_type) {
        ogs_error("[%s] No User-Agent", request->h.uri);

        scp_assoc_remove(assoc);
        return OGS_ERROR;
    }

    if (target_nf_type || service_type) {
        if (!target_nf_type || !service_type) {
            ogs_error("[%s] No Mandatory Discovery [%d:%d]",
                request->h.uri, target_nf_type, service_type);

            scp_assoc_remove(assoc);
            return OGS_ERROR;
        }

        if (target_nf_type == OpenAPI_nf_type_NRF)
            client = NF_INSTANCE_CLIENT(ogs_sbi_self()->nrf_instance);
        else {
            /* ===== TYcustom: UDR UE-affinity routing (imsi % N) at the request
             * ENTRY point ==========================================================
             * The caller (UDM/PCF) caches the UDR it discovered once and then puts
             * Target-nf-instance-id on every subsequent request, so those requests
             * are sent directly (line ~455) and NEVER reach the discover callback
             * where the original routing lived. Do the routing here, BEFORE the
             * Target-nf-instance-id lookup, and deliberately OVERRIDE the caller's
             * pinned instance so a UE's imsi always maps to the same UDR.
             *
             * Gate on target==UDR (covers UDM->UDR and PCF->UDR). If we cannot
             * extract an imsi, or have no locally-known UDR candidate, fall through
             * to the stock logic below (no crash, no dropped request). */
            if (target_nf_type == OpenAPI_nf_type_UDR) {
                ogs_sbi_nf_instance_t *udr_arr[SCP_MAX_UDR];
                char imsi_digits[32];

                if (scp_udr_extract_imsi_digits(
                        request->h.uri, imsi_digits, sizeof(imsi_digits))) {
                    int n_udr = scp_collect_sorted_udr(
                            target_nf_type, requester_nf_type,
                            discovery_option, udr_arr, SCP_MAX_UDR);
                    if (n_udr > 0) {
                        uint64_t ueid =
                            ogs_uint64_from_string_decimal(imsi_digits);
                        int idx = (int)(ueid % (uint64_t)n_udr);
                        ogs_sbi_nf_instance_t *chosen = udr_arr[idx];
                        ogs_sbi_client_t *udr_client =
                            ogs_sbi_client_find_by_service_type(
                                    chosen, service_type);
                        if (udr_client) {
                            nf_instance = chosen;
                            client = udr_client;
                            ogs_info("[SCP-UDR-ROUTE] imsi=%s n_udr=%d idx=%d "
                                    "-> %s", imsi_digits, n_udr, idx,
                                    scp_udr_sort_key(chosen));
                        } else {
                            ogs_warn("[SCP-UDR-ROUTE] no client for chosen UDR "
                                    "%s (imsi=%s), fallback",
                                    scp_udr_sort_key(chosen), imsi_digits);
                        }
                    } else {
                        ogs_warn("[SCP-UDR-ROUTE] no UDR candidate for imsi=%s, "
                                "fallback", imsi_digits);
                    }
                }
            }

            /* Stock path: honor Target-nf-instance-id only if the UDR-affinity
             * routing above did not already pick a client. */
            if (!client &&
                    discovery_option && discovery_option->target_nf_instance_id) {
                nf_instance = ogs_sbi_nf_instance_find(
                        discovery_option->target_nf_instance_id);
                if (nf_instance) {
                    client = ogs_sbi_client_find_by_service_type(
                                nf_instance, service_type);
                    if (!client) {
                        ogs_error("[%s] Cannot find client "
                                "[type:%s target_nf_type:%s service_name:%s]",
                                nf_instance->id,
                                OpenAPI_nf_type_ToString(nf_instance->nf_type),
                                OpenAPI_nf_type_ToString(target_nf_type),
                                ogs_sbi_service_type_to_name(service_type));
                    }
                }
            }
        }

        discovery_presence = true;
    }

    /**************************************
     * Send REQUEST message to the Next-SCP
     **************************************/
    if (next_scp) {

        if (false == send_request(
                    next_scp, response_handler, request, true, assoc)) {
            ogs_error("send_request() failed");

            scp_assoc_remove(assoc);
            return OGS_ERROR;
        }

        return OGS_OK;
    }

    /************************************
     * Send REQUEST message to the CLIENT
     ************************************/
    if (headers.target_apiroot || client) {

        /**************************
         * Check if SEPP is needed
         **************************/
        if (headers.target_apiroot &&
            ogs_sbi_fqdn_in_vplmn(headers.target_apiroot) == true) {

            /* Re-Use Custom Header(Target-apiRoot) from Target-apiRoot */
            ogs_assert(!assoc->target_apiroot);
            assoc->target_apiroot = ogs_strdup(headers.target_apiroot);
            ogs_assert(assoc->target_apiroot);

        } else if (client && client->fqdn &&
            ogs_sbi_fqdn_in_vplmn(client->fqdn) == true) {

            /* Generate Custom Header(Target-apiRoot) from Known-Client */
            ogs_assert(!assoc->target_apiroot);
            assoc->target_apiroot = ogs_sbi_client_apiroot(client);
            ogs_assert(assoc->target_apiroot);
        }

        if (assoc->target_apiroot) {

            /* Visited Network requires SEPP */
            sepp_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->sepp_instance);

            if (!sepp_client && !nrf_client) {

                ogs_error("No SEPP(%p) and NRF(%p) [%s]",
                        sepp_client, nrf_client, assoc->target_apiroot);

                scp_assoc_remove(assoc);
                return OGS_ERROR;

            } else if (!sepp_client) {

                assoc->request = request;
                ogs_assert(assoc->request);

                assoc->target_nf_type = OpenAPI_nf_type_SEPP;;
                ogs_assert(assoc->request);
                assoc->requester_nf_type = requester_nf_type;
                ogs_assert(assoc->request);

                if (false == send_discover(
                            nrf_client, sepp_discover_handler, assoc)) {
                    ogs_error("send_discover() failed");

                    scp_assoc_remove(assoc);
                    return OGS_ERROR;
                }

                return OGS_OK;
            }
        }

        if (sepp_client) {

            /* Switch to the SEPP client */
            client = sepp_client;

        } else if (headers.target_apiroot) {
            bool rc;
            OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
            char *fqdn = NULL;
            uint16_t fqdn_port = 0;
            ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

            /* Find or Add Client Instance */
            rc = ogs_sbi_getaddr_from_uri(
                    &scheme, &fqdn, &fqdn_port, &addr, &addr6,
                    headers.target_apiroot);
            if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
                ogs_error("Invalid Target-apiRoot [%s]",
                        headers.target_apiroot);

                scp_assoc_remove(assoc);
                return OGS_ERROR;
            }

            client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
            if (!client) {
                ogs_debug("%s: ogs_sbi_client_add()", OGS_FUNC);
                client = ogs_sbi_client_add(
                        scheme, fqdn, fqdn_port, addr, addr6);
                if (!client) {
                    ogs_error("%s: ogs_sbi_client_add() failed", OGS_FUNC);

                    ogs_free(fqdn);
                    ogs_freeaddrinfo(addr);
                    ogs_freeaddrinfo(addr6);
                    scp_assoc_remove(assoc);

                    return OGS_ERROR;
                }
            }
            OGS_SBI_SETUP_CLIENT(assoc, client);

            ogs_free(fqdn);
            ogs_freeaddrinfo(addr);
            ogs_freeaddrinfo(addr6);
        }

        ogs_assert(client);

        if (false == send_request(
                    client, response_handler, request, false, assoc)) {
            ogs_error("send_request() failed");

            scp_assoc_remove(assoc);
            return OGS_ERROR;
        }

        return OGS_OK;
    }

    /*******************************
     * Send DISCOVERY message to NRF
     *******************************/
    if (discovery_presence == true) {

        if (headers.nrf_uri) {
            char *key = NULL;
            char *nnrf_disc = NULL;
            char *nnrf_nfm = NULL;
            char *nnrf_oauth2 = NULL;

            char *tmp = NULL, *p = NULL;
            char *v_start = NULL, *v_end = NULL;

            tmp = ogs_strdup(headers.nrf_uri);
            ogs_assert(tmp);

            for (key = ogs_strtok_r(tmp, ": ", &p);
                    key != NULL; key = ogs_strtok_r(NULL, ": ", &p)) {

                v_start = v_end = NULL;

                while (*p) {
                    if (*p == ';') {
                        if ((v_start && v_end) || !v_start) {
                            p++;
                            break;
                        }
                    } else if (*p == '"') {
                        if (!v_start) v_start = p+1;
                        else if (!v_end) v_end = p;
                    }
                    p++;
                }

                if (v_start && v_end) {
                    SWITCH(key)
                    CASE(OGS_SBI_SERVICE_NAME_NNRF_NFM)
                        nnrf_nfm = ogs_strndup(v_start, v_end-v_start);
                        break;
                    CASE(OGS_SBI_SERVICE_NAME_NNRF_DISC)
                        nnrf_disc = ogs_strndup(v_start, v_end-v_start);
                        break;
                    CASE(OGS_SBI_SERVICE_NAME_NNRF_OAUTH2)
                        nnrf_oauth2 = ogs_strndup(v_start, v_end-v_start);
                        break;
                    DEFAULT
                    END
                }
            }

            ogs_free(tmp);

            /* Find or Add Client Instance */
            if (nnrf_disc) {
                bool rc;
                OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
                char *fqdn = NULL;
                uint16_t fqdn_port = 0;
                ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

                rc = ogs_sbi_getaddr_from_uri(
                        &scheme, &fqdn, &fqdn_port, &addr, &addr6, nnrf_disc);
                if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
                    ogs_error("Invalid nnrf-disc [%s]", nnrf_disc);

                    scp_assoc_remove(assoc);
                    return OGS_ERROR;
                }

                nrf_client = ogs_sbi_client_find(
                        scheme, fqdn, fqdn_port, addr, addr6);
                if (!nrf_client) {
                    ogs_debug("%s: ogs_sbi_client_add()", OGS_FUNC);
                    nrf_client = ogs_sbi_client_add(
                            scheme, fqdn, fqdn_port, addr, addr6);
                    if (!nrf_client) {
                        ogs_error("%s: ogs_sbi_client_add()", OGS_FUNC);

                        ogs_free(fqdn);
                        ogs_freeaddrinfo(addr);
                        ogs_freeaddrinfo(addr6);
                        scp_assoc_remove(assoc);

                        return OGS_ERROR;
                    }
                }
                OGS_SBI_SETUP_CLIENT(assoc, nrf_client);

                ogs_free(fqdn);
                ogs_freeaddrinfo(addr);
                ogs_freeaddrinfo(addr6);
            }

            if (nnrf_nfm) ogs_free(nnrf_nfm);
            if (nnrf_disc) ogs_free(nnrf_disc);
            if (nnrf_oauth2) ogs_free(nnrf_oauth2);
        }

        if (!nrf_client) {
            ogs_error("No NRF");

            scp_assoc_remove(assoc);
            return OGS_ERROR;
        }

        if (!discovery_option->num_of_service_names) {
            ogs_error("No service names");
            scp_assoc_remove(assoc);
            return OGS_ERROR;
        } else if (discovery_option->num_of_service_names > 1) {
    /*
     * TS29.500
     * 6.10.3 NF Discovery and Selection for indirect communication
     *        with Delegated Discovery
     * 6.10.3.2 Conveyance of NF Discovery Factors
     *
     * If the NF service consumer includes more than one service name in the
     * 3gpp-Sbi-Discovery-service-names header, the service name corresponding
     * to the service request shall be listed as the first service name
     * in the header.
     *
     * NOTE 3: The SCP can assume that the service request corresponds
     *         to the first service name in the header.
     */
            int i;

            for (i = 1; i < discovery_option->num_of_service_names; i++)
                ogs_free(discovery_option->service_names[i]);
            discovery_option->num_of_service_names = 1;

            ogs_error("NOTE 3: The SCP can assume that the service request "
                    "corresponds to the first service name in the header "
                    "in TS29.500");
        }

        assoc->request = request;
        ogs_assert(assoc->request);
        assoc->service_type = service_type;
        ogs_assert(assoc->service_type);

        assoc->target_nf_type = target_nf_type;
        ogs_assert(assoc->target_nf_type);
        assoc->requester_nf_type = requester_nf_type;
        ogs_assert(assoc->requester_nf_type);

        if (false == send_discover(nrf_client, nf_discover_handler, assoc)) {
            ogs_error("send_discover() failed");
            scp_assoc_remove(assoc);
            return OGS_ERROR;
        }

        return OGS_OK;
    }

    scp_assoc_remove(assoc);

    /***************************************
     * Receive NOTIFICATION message from NRF
     ***************************************/
    ogs_assert(request);
    ogs_assert(data);

    e = scp_event_new(OGS_EVENT_SBI_SERVER);
    ogs_assert(e);

    e->h.sbi.request = request;
    e->h.sbi.data = data;

    rv = ogs_queue_push(ogs_app()->queue, e);
    if (rv != OGS_OK) {
        ogs_error("ogs_queue_push() failed:%d", (int)rv);

        ogs_event_free(e);
        return OGS_ERROR;
    }

    return OGS_OK;
}

static int response_handler(
        int status, ogs_sbi_response_t *response, void *data)
{
    scp_assoc_t *assoc = data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;

    ogs_assert(assoc);

    stream_id = assoc->stream_id;
    ogs_assert(stream_id >= OGS_MIN_POOL_ID && stream_id <= OGS_MAX_POOL_ID);
    stream = ogs_sbi_stream_find_by_id(stream_id);

    if (status != OGS_OK) {

        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "response_handler() failed [%d]", status);

        scp_assoc_remove(assoc);

        if (stream) {
            ogs_assert(true ==
                ogs_sbi_server_send_error(stream,
                    OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL,
                    "response_handler() failed", NULL, NULL));
        } else
            ogs_error("STREAM has already been removed [%d]", stream_id);

        return OGS_ERROR;
    }

    ogs_assert(response);

    if (assoc->nf_service_producer) {
        if (assoc->nf_service_producer->id)
            ogs_sbi_header_set(response->http.headers,
                OGS_SBI_CUSTOM_PRODUCER_ID, assoc->nf_service_producer->id);
        else
            ogs_error("No NF-Instance ID");
    }

    scp_assoc_remove(assoc);

    if (!stream) {
        ogs_error("STREAM has already been removed [%d]", stream_id);
        ogs_sbi_response_free(response);
        return OGS_ERROR;
    }
    ogs_expect(true == ogs_sbi_server_send_response(stream, response));

    return OGS_OK;
}

static int nf_discover_handler(
        int status, ogs_sbi_response_t *response, void *data)
{
    int rv, res_status;
    char *strerror = NULL;
    ogs_sbi_message_t message;

    scp_assoc_t *assoc = data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;

    ogs_sbi_request_t *request = NULL;
    ogs_sbi_service_type_e service_type = OGS_SBI_SERVICE_TYPE_NULL;

    OpenAPI_nf_type_e target_nf_type = OpenAPI_nf_type_NULL;
    OpenAPI_nf_type_e requester_nf_type = OpenAPI_nf_type_NULL;
    ogs_sbi_discovery_option_t *discovery_option = NULL;

    ogs_sbi_nf_instance_t *nf_instance = NULL;
    ogs_sbi_client_t *client = NULL;
    ogs_sbi_client_t *sepp_client = NULL;

    ogs_assert(assoc);
    request = assoc->request;
    ogs_assert(request);
    service_type = assoc->service_type;
    ogs_assert(service_type);

    target_nf_type = assoc->target_nf_type;
    ogs_assert(target_nf_type);
    requester_nf_type = assoc->requester_nf_type;
    ogs_assert(requester_nf_type);
    discovery_option = assoc->discovery_option;
    ogs_assert(discovery_option);

    stream_id = assoc->stream_id;
    ogs_assert(stream_id >= OGS_MIN_POOL_ID && stream_id <= OGS_MAX_POOL_ID);
    stream = ogs_sbi_stream_find_by_id(stream_id);

    if (status != OGS_OK) {

        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "nf_discover_handler() failed [%d]", status);

        scp_assoc_remove(assoc);

        if (stream) {
            ogs_assert(true ==
                ogs_sbi_server_send_error(stream,
                    OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL,
                    "nf_discover_handler() failed", NULL, NULL));
        } else
            ogs_error("STREAM has already been removed [%d]", stream_id);

        return OGS_ERROR;
    }

    ogs_assert(response);

    rv = ogs_sbi_parse_response(&message, response);
    if (rv != OGS_OK) {
        strerror = ogs_msprintf("cannot parse HTTP response");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (message.res_status != OGS_SBI_HTTP_STATUS_OK) {
        strerror = ogs_msprintf("NF-Discover failed [%d]", message.res_status);
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!message.SearchResult) {
        strerror = ogs_msprintf("No SearchResult");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    ogs_nnrf_disc_handle_nf_discover_search_result(message.SearchResult);

    /* ===== TYcustom: UDR UE-affinity routing (ueid % N) ====================
     * Only when target is UDR and an imsi can be extracted from the URI, pick
     * the UDR deterministically by idx = imsi % N (N = live UDR count, sorted
     * by pod IP). Otherwise fall through to the original selection. The gate is
     * target==UDR so it covers BOTH UDM->UDR and PCF->UDR. */
    nf_instance = NULL;
    if (target_nf_type == OpenAPI_nf_type_UDR) {
        ogs_sbi_nf_instance_t *udr_arr[SCP_MAX_UDR];
        char imsi_digits[32];
        int n_udr;

        if (scp_udr_extract_imsi_digits(
                request->h.uri, imsi_digits, sizeof(imsi_digits))) {
            n_udr = scp_collect_sorted_udr(
                    target_nf_type, requester_nf_type, discovery_option,
                    udr_arr, SCP_MAX_UDR);
            if (n_udr > 0) {
                uint64_t ueid = strtoull(imsi_digits, NULL, 10);
                int idx = (int)(ueid % (uint64_t)n_udr);
                nf_instance = udr_arr[idx];
                ogs_info("[SCP-UDR-ROUTE] imsi=%s n_udr=%d idx=%d -> %s",
                        imsi_digits, n_udr, idx,
                        scp_udr_sort_key(nf_instance));
            } else {
                ogs_warn("[SCP-UDR-ROUTE] no UDR candidate for imsi=%s",
                        imsi_digits);
            }
        }
    }

    /* fallback: non-UDR / no imsi / no UDR candidate -> original selection */
    if (!nf_instance)
        nf_instance = ogs_sbi_nf_instance_find_by_discovery_param(
                target_nf_type, requester_nf_type, discovery_option);
    if (!nf_instance) {
        strerror = ogs_msprintf("(NF discover) No NF-Instance [%s:%s]",
                    ogs_sbi_service_type_to_name(service_type),
                    OpenAPI_nf_type_ToString(requester_nf_type));
        res_status = OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT;
        goto cleanup;
    }

    /* Store NF Service Producer */
    assoc->nf_service_producer = nf_instance;
    ogs_assert(assoc->nf_service_producer);

    client = ogs_sbi_client_find_by_service_type(nf_instance, service_type);
    if (!client) {
        strerror = ogs_msprintf("(NF discover) No client [%s:%s]",
                    ogs_sbi_service_type_to_name(service_type),
                    OpenAPI_nf_type_ToString(requester_nf_type));
        res_status = OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT;
        goto cleanup;
    }

    /**************************
     * Check if SEPP is needed
     **************************/
    if (client->fqdn && ogs_sbi_fqdn_in_vplmn(client->fqdn) == true) {

        /* Visited Network requires SEPP */
        sepp_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->sepp_instance);
        if (!sepp_client) {
            ogs_error("No SEPP [%s]", client->fqdn);
            strerror = ogs_msprintf("No SEPP [%s]", client->fqdn);
            res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }

        /* Generate Custom Header(Target-apiRoot) from Known-Client */
        ogs_assert(!assoc->target_apiroot);
        assoc->target_apiroot = ogs_sbi_client_apiroot(client);
        ogs_assert(assoc->target_apiroot);

        client = sepp_client;
    }

    if (false == send_request(
                client, response_handler, request, false, assoc)) {
        strerror = ogs_msprintf("send_request() failed");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    ogs_sbi_response_free(response);
    ogs_sbi_message_free(&message);

    return OGS_OK;

cleanup:
    ogs_assert(strerror);
    ogs_error("%s", strerror);

    scp_assoc_remove(assoc);

    if (stream) {
        ogs_assert(true == ogs_sbi_server_send_error(
                stream, res_status, NULL, strerror, NULL, NULL));
    } else
        ogs_error("STREAM has already been removed [%d]", stream_id);

    ogs_free(strerror);

    ogs_sbi_response_free(response);
    ogs_sbi_message_free(&message);

    return OGS_ERROR;
}

static int sepp_discover_handler(
        int status, ogs_sbi_response_t *response, void *data)
{
    int rv, res_status;
    char *strerror = NULL;
    ogs_sbi_message_t message;

    scp_assoc_t *assoc = data;
    ogs_sbi_stream_t *stream = NULL;
    ogs_pool_id_t stream_id = OGS_INVALID_POOL_ID;

    ogs_sbi_request_t *request = NULL;

    ogs_sbi_client_t *sepp_client = NULL;

    ogs_assert(assoc);

    stream_id = assoc->stream_id;
    ogs_assert(stream_id >= OGS_MIN_POOL_ID && stream_id <= OGS_MAX_POOL_ID);
    stream = ogs_sbi_stream_find_by_id(stream_id);

    if (status != OGS_OK) {

        ogs_log_message(
                status == OGS_DONE ? OGS_LOG_DEBUG : OGS_LOG_WARN, 0,
                "sepp_discover_handler() failed [%d]", status);

        scp_assoc_remove(assoc);

        if (stream) {
            ogs_assert(true ==
                ogs_sbi_server_send_error(stream,
                    OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR, NULL,
                    "sepp_discover_handler() failed", NULL, NULL));
        } else
            ogs_error("STREAM has already been removed [%d]", stream_id);

        return OGS_ERROR;
    }

    ogs_assert(response);

    rv = ogs_sbi_parse_response(&message, response);
    if (rv != OGS_OK) {
        strerror = ogs_msprintf("cannot parse HTTP response");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (message.res_status != OGS_SBI_HTTP_STATUS_OK) {
        strerror = ogs_msprintf("NF-Discover failed [%d]", message.res_status);
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!message.SearchResult) {
        strerror = ogs_msprintf("No SearchResult");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    ogs_nnrf_disc_handle_nf_discover_search_result(message.SearchResult);

    /*****************************
     * Check if SEPP is discovered
     *****************************/
    sepp_client = NF_INSTANCE_CLIENT(ogs_sbi_self()->sepp_instance);
    if (!sepp_client) {
        strerror = ogs_msprintf("No SEPP");
        res_status = OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT;
        goto cleanup;
    }

    ogs_assert(assoc->target_apiroot);
    request = assoc->request;
    ogs_assert(request);

    if (false == send_request(
                sepp_client, response_handler, request, false, assoc)) {
        strerror = ogs_msprintf("send_request() failed");
        res_status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    ogs_sbi_response_free(response);
    ogs_sbi_message_free(&message);

    return OGS_OK;

cleanup:
    ogs_assert(strerror);
    ogs_error("%s", strerror);

    scp_assoc_remove(assoc);

    if (stream) {
        ogs_assert(true == ogs_sbi_server_send_error(
                stream, res_status, NULL, strerror, NULL, NULL));
    } else
        ogs_error("STREAM has already been removed [%d]", stream_id);

    ogs_free(strerror);

    ogs_sbi_response_free(response);
    ogs_sbi_message_free(&message);

    return OGS_ERROR;
}

static bool send_discover(
        ogs_sbi_client_t *client, ogs_sbi_client_cb_f client_cb,
        scp_assoc_t *assoc)
{
    bool rc;
    ogs_sbi_request_t *request = NULL;

    ogs_assert(client);
    ogs_assert(assoc);

    request = ogs_nnrf_disc_build_discover(
                assoc->target_nf_type, assoc->requester_nf_type,
                assoc->target_nf_type != OpenAPI_nf_type_SEPP ?
                    assoc->discovery_option : NULL);
    if (!request) {
        ogs_error("ogs_nnrf_disc_build_discover() failed");
        return false;
    }

    rc = ogs_sbi_client_send_request(client, client_cb, request, assoc);
    ogs_expect(rc == true);

    ogs_sbi_request_free(request);

    return rc;
}

static bool send_request(
        ogs_sbi_client_t *client, ogs_sbi_client_cb_f client_cb,
        ogs_sbi_request_t *request, bool do_not_remove_custom_header,
        scp_assoc_t *assoc)
{
    bool rc;
    ogs_sbi_request_t scp_request;
    char *uri_apiroot = NULL;

    ogs_assert(client);
    ogs_assert(request);
    ogs_assert(assoc);

    /* Copy Request for sending SCP */
    copy_request(&scp_request, request, do_not_remove_custom_header);
    ogs_assert(scp_request.http.headers);

    /* Added Custom Header(Target-apiRoot) */
    if (assoc->target_apiroot)
        ogs_sbi_header_set(scp_request.http.headers,
                OGS_SBI_CUSTOM_TARGET_APIROOT, assoc->target_apiroot);

    /* Client ApiRoot */
    uri_apiroot = ogs_sbi_client_apiroot(client);
    ogs_assert(uri_apiroot);

    /* Setup New URI */
    scp_request.h.uri = ogs_msprintf("%s%s", uri_apiroot, request->h.uri);
    ogs_assert(scp_request.h.uri);

    /* Send the HTTP Request with New URI and HTTP Headers */
    rc = ogs_sbi_client_send_request(client, client_cb, &scp_request, assoc);
    ogs_expect(rc == true);

    ogs_sbi_http_hash_free(scp_request.http.headers);
    ogs_free(scp_request.h.uri);
    ogs_free(uri_apiroot);

    return rc;
}

static void copy_request(
        ogs_sbi_request_t *target, ogs_sbi_request_t *source,
        bool do_not_remove_custom_header)
{
    ogs_hash_index_t *hi;

    ogs_assert(source);
    ogs_assert(target);

    memset(target, 0, sizeof(*target));

    /* HTTP method/params/content */
    target->h.method = source->h.method;
    target->http.params = source->http.params;
    target->http.content = source->http.content;
    target->http.content_length = source->http.content_length;

    /* HTTP Headers
     *
     * To remove the followings,
     *   Scheme - https
     *   Authority - scp.open5gs.org
     */
    target->http.headers = ogs_hash_make();
    ogs_assert(target->http.headers);

    /* Extract HTTP Header */
    for (hi = ogs_hash_first(source->http.headers);
            hi; hi = ogs_hash_next(hi)) {
        char *key = (char *)ogs_hash_this_key(hi);
        char *val = ogs_hash_this_val(hi);

        if (!key || !val) {
            ogs_error("No Key[%s] Value[%s]", key, val);
            continue;
        }

        /*
         * <RFC 2616>
         *  Each header field consists of a name followed by a colon (":")
         *  and the field value. Field names are case-insensitive.
         */
        if (do_not_remove_custom_header == false &&
            !strcasecmp(key, OGS_SBI_CUSTOM_TARGET_APIROOT)) {
        } else if (do_not_remove_custom_header == false &&
            !strncasecmp(key, OGS_SBI_CUSTOM_DISCOVERY_COMMON,
                strlen(OGS_SBI_CUSTOM_DISCOVERY_COMMON))) {
        } else if (!strcasecmp(key, OGS_SBI_SCHEME)) {
        } else if (!strcasecmp(key, OGS_SBI_AUTHORITY)) {
        } else {
            ogs_sbi_header_set(target->http.headers, key, val);
        }
    }
}
