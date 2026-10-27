#include "kid_app/esp_pet_https_transport.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace kid_app {
namespace {
constexpr double kMaxExactJsonInteger = 9007199254740991.0;

bool ValidDeviceId(const char* id) {
    if (!id) return false;
    const size_t n=std::strlen(id);
    if (n<3 || n>63) return false;
    for (size_t i=0;i<n;++i) {
        char c=id[i];
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
              (c>='0'&&c<='9')||(i && (c=='-'||c=='_'))))
            return false;
    }
    return true;
}

bool SafeHttpsOrigin(const char* origin) {
    if (!origin || std::strncmp(origin,"https://",8)!=0) return false;
    const char* host=origin+8;
    if (!*host || std::strlen(origin)>155) return false;
    // No paths, URL credentials, fragments, query, whitespace or CRLF.
    for (const char* p=host;*p;++p) {
        char c=*p;
        if (!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||
              (c>='0'&&c<='9')||c=='.'||c=='-'||c==':' ||
              c=='['||c==']')) return false;
    }
    return true;
}

bool ValidToken(const char* token) {
    if (!token) return false;
    size_t size=0;
    for (;size<191 && token[size];++size)
        if (token[size]<=32 || static_cast<unsigned char>(token[size])>=127)
            return false;
    return size>=32 && size<191 && token[size]=='\0';
}

bool JsonInteger(const cJSON* node, uint64_t* out) {
    if (!out || !cJSON_IsNumber(node)) return false;
    const double n=node->valuedouble;
    if (!std::isfinite(n) || n<1 || n>kMaxExactJsonInteger || std::floor(n)!=n)
        return false;
    *out=static_cast<uint64_t>(n);
    return true;
}
}  // namespace

EspPetHttpsTransport::EspPetHttpsTransport(
        const char* origin, const char* device_id, const char* device_token) {
    if (!SafeHttpsOrigin(origin) || !ValidDeviceId(device_id) ||
        !ValidToken(device_token)) return;
    const int url_len=std::snprintf(url_,sizeof(url_),
        "%s/v1/devices/%s/events",origin,device_id);
    const int auth_len=std::snprintf(auth_,sizeof(auth_),"Bearer %s",device_token);
    configured_=url_len>0 && auth_len>0 &&
        static_cast<size_t>(url_len)<sizeof(url_) &&
        static_cast<size_t>(auth_len)<sizeof(auth_);
    if (!configured_) {
        url_[0]=0;
        auth_[0]=0;
    }
}

esp_err_t EspPetHttpsTransport::OnHttpEvent(esp_http_client_event_t* event) {
    if (!event || !event->user_data) return ESP_FAIL;
    Reply* reply=static_cast<Reply*>(event->user_data);
    if (event->event_id!=HTTP_EVENT_ON_DATA) return ESP_OK;
    if (event->data_len<0 || (!event->data && event->data_len)) {
        reply->overflow=true;
        return ESP_FAIL;
    }
    const size_t size=static_cast<size_t>(event->data_len);
    if (reply->overflow || size>sizeof(reply->bytes)-1-reply->used) {
        reply->overflow=true;
        return ESP_FAIL;
    }
    if (size) std::memcpy(reply->bytes+reply->used,event->data,size);
    reply->used+=size;
    reply->bytes[reply->used]=0;
    return ESP_OK;
}

bool EspPetHttpsTransport::ParseAck(
        const Reply& response, PetDeviceBridge::ServerAck* out) {
    if (!out || response.overflow || response.used==0) return false;
    cJSON* root=cJSON_ParseWithLength(response.bytes,response.used);
    if (!root) return false;
    bool valid=false;
    do {
        if (!cJSON_IsObject(root)) break;
        const cJSON* list=cJSON_GetObjectItemCaseSensitive(root,"acks");
        if (!cJSON_IsArray(list) || cJSON_GetArraySize(list)!=1) break;
        const cJSON* receipt=cJSON_GetArrayItem(list,0);
        if (!cJSON_IsObject(receipt)) break;
        const cJSON* id=cJSON_GetObjectItemCaseSensitive(receipt,"message_id");
        const cJSON* seq=cJSON_GetObjectItemCaseSensitive(receipt,"seq");
        const cJSON* accepted=cJSON_GetObjectItemCaseSensitive(receipt,"accepted");
        const cJSON* version=cJSON_GetObjectItemCaseSensitive(receipt,"server_state_version");
        const cJSON* reason=cJSON_GetObjectItemCaseSensitive(receipt,"reason_code");
        if (!cJSON_IsString(id) || !id->valuestring ||
            !cJSON_IsTrue(accepted)) break;
        uint64_t seq_no=0, server_version=0;
        if (!JsonInteger(seq,&seq_no) || !JsonInteger(version,&server_version))
            break;
        const size_t len=std::strlen(id->valuestring);
        if (len<3 || len>=sizeof(message_id_)) break;
        bool duplicate=false;
        if (reason && !cJSON_IsNull(reason)) {
            if (!cJSON_IsString(reason) || !reason->valuestring ||
                std::strcmp(reason->valuestring,"DUPLICATE")!=0)
                break;
            duplicate=true;
        }
        std::memcpy(message_id_,id->valuestring,len+1);
        out->message_id=message_id_;
        out->sequence=seq_no;
        out->server_version=server_version;
        out->accepted=true;
        out->duplicate=duplicate;
        valid=true;
    } while (false);
    cJSON_Delete(root);
    return valid;
}

bool EspPetHttpsTransport::PostJson(
        const char* body, size_t size, PetDeviceBridge::ServerAck* ack) {
    if (!configured_ || !body || !size || size>=PetDeviceBridge::kJsonMaxBytes ||
        !ack) return false;

#if !defined(CONFIG_MBEDTLS_CERTIFICATE_BUNDLE) || !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    // Never silently run TLS without a trusted certificate chain.
    return false;
#else
    Reply response{};
    esp_http_client_config_t config{};
    config.url=url_;
    config.method=HTTP_METHOD_POST;
    config.event_handler=&EspPetHttpsTransport::OnHttpEvent;
    config.user_data=&response;
    config.timeout_ms=4000;
    config.disable_auto_redirect=true;
    config.max_authorization_retries=-1;
    config.crt_bundle_attach=esp_crt_bundle_attach;
    config.skip_cert_common_name_check=false;
    esp_http_client_handle_t client=esp_http_client_init(&config);
    if (!client) return false;
    const bool setup=esp_http_client_set_header(client,"Content-Type","application/json")==ESP_OK &&
        esp_http_client_set_header(client,"Authorization",auth_)==ESP_OK &&
        esp_http_client_set_post_field(client,body,static_cast<int>(size))==ESP_OK;
    bool okay=false;
    if (setup && esp_http_client_perform(client)==ESP_OK &&
        esp_http_client_get_status_code(client)==200 &&
        !response.overflow) {
        okay=ParseAck(response,ack);
    }
    esp_http_client_cleanup(client);
    return okay;
#endif
}

}  // namespace kid_app
