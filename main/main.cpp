#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "driver/gpio.h"
#include "SdUsbManager.hpp"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

static const char *TAG = "Updater";
#define BOOT_BTN_PIN GPIO_NUM_0

static lv_obj_t * scr_splash = NULL;
static lv_obj_t * scr_wifi_list = NULL;
static lv_obj_t * scr_password = NULL;
static lv_obj_t * scr_store = NULL;

static lv_obj_t * list_wifi = NULL;
static lv_obj_t * ta_wifi_pass = NULL;
static lv_obj_t * label_wifi_title = NULL;
static char current_ssid[33] = {0};

static lv_obj_t * tv_store = NULL;
static lv_obj_t * tab_loja = NULL;
static lv_obj_t * tab_updates = NULL;
static lv_obj_t * overlay_loading = NULL;
static lv_obj_t * lbl_status = NULL;
static lv_obj_t * spinner = NULL;

static lv_obj_t * lbl_splash_version = NULL;

static cJSON * pending_updates = NULL;
static cJSON * job_queue = NULL;
static cJSON * ui_packages = NULL;

LV_IMAGE_DECLARE(icon_updater); 

static void load_catalog_task(void *pvParameters);

// ==========================================
// ESTILOS GLOBAIS E UI BASE
// ==========================================
static void style_dark_ta(lv_obj_t * ta) {
    lv_obj_set_style_bg_color(ta, lv_color_hex(0x222222), 0);
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_border_width(ta, 2, (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_white(), (lv_style_selector_t)LV_PART_CURSOR | (lv_style_selector_t)LV_STATE_FOCUSED);
}

static void style_dark_kb(lv_obj_t * kb) {
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x111111), LV_PART_MAIN); 
    lv_obj_set_style_border_width(kb, 0, LV_PART_MAIN); 
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x333333), LV_PART_ITEMS); 
    lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS); 
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS); 
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x444444), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED); 
    lv_obj_set_style_text_color(kb, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED); 
    lv_obj_set_style_pad_left(kb, 15, 0);
    lv_obj_set_style_pad_right(kb, 15, 0);
    lv_obj_set_style_pad_bottom(kb, 15, 0);
}

// ==========================================
// FUNÇÕES DE HARDWARE & FLASH SEGURO
// ==========================================
static void return_to_factory() {
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        bsp_display_brightness_set(0); 
        bsp_display_unlock();
    }
    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory_part) esp_ota_set_boot_partition(factory_part);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void clear_i2c_bus(void) {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT_OD;
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_14) | (1ULL << GPIO_NUM_15);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);
    for (int i = 0; i < 9; i++) {
        gpio_set_level(GPIO_NUM_14, 0); esp_rom_delay_us(100);
        gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    }
    gpio_set_level(GPIO_NUM_15, 0); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_14, 1); esp_rom_delay_us(100);
    gpio_set_level(GPIO_NUM_15, 1); esp_rom_delay_us(100);
    gpio_reset_pin(GPIO_NUM_14);
    gpio_reset_pin(GPIO_NUM_15);
}

static bool flash_factory_from_sd() {
    FILE* f = fopen("/sdcard/factory.bin", "rb");
    if (!f) return false;
    
    fseek(f, 0, SEEK_END);
    size_t file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t magic;
    fread(&magic, 1, 1, f);
    if (magic != 0xE9) { 
        ESP_LOGE(TAG, "Arquivo de Firmware Invalido! (Magic Byte != 0xE9)");
        fclose(f); 
        return false; 
    }

    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (!factory_part || file_size > factory_part->size) {
        ESP_LOGE(TAG, "Particao invalida ou firmware grande demais!");
        fclose(f);
        return false;
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_label_set_text(lbl_status, "Gravando Memoria Flash...\nNao desligue o relogio!");
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "Apagando particao Factory...");
    if (esp_partition_erase_range(factory_part, 0, factory_part->size) != ESP_OK) {
        fclose(f); return false;
    }

    ESP_LOGI(TAG, "Gravando arquivo na Flash...");
    size_t written = 0;
    uint8_t *buf = (uint8_t*)heap_caps_malloc(16384, MALLOC_CAP_INTERNAL);
    fseek(f, 0, SEEK_SET);
    
    while(written < file_size) {
        size_t to_read = (file_size - written > 16384) ? 16384 : (file_size - written);
        fread(buf, 1, to_read, f);
        if (esp_partition_write(factory_part, written, buf, to_read) != ESP_OK) {
            heap_caps_free(buf); fclose(f); return false;
        }
        written += to_read;
        if (written % (16384 * 4) == 0 && bsp_display_lock(pdMS_TO_TICKS(10))) {
            lv_label_set_text_fmt(lbl_status, "Gravando Flash: %d %%", (written * 100) / file_size);
            bsp_display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    heap_caps_free(buf);
    fclose(f);
    
    esp_ota_set_boot_partition(factory_part);
    return true;
}

// ==========================================
// FUNÇÃO PARA LER A VERSÃO ATUAL DO SD
// ==========================================
static void get_app_version_from_sd(const char* app_id, char* out_version, size_t max_len) {
    strncpy(out_version, "0.0.0", max_len); // Versão padrão caso dê erro ou não encontre
    FILE *vf = fopen("/sdcard/apps/versions.json", "r");
    if (vf) {
        fseek(vf, 0, SEEK_END);
        long fsize = ftell(vf);
        fseek(vf, 0, SEEK_SET);
        if(fsize > 0) {
            char *jstr = (char*)heap_caps_malloc(fsize + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            fread(jstr, 1, fsize, vf);
            jstr[fsize] = 0;
            char *ptr = jstr;
            if ((unsigned char)ptr[0] == 0xEF && (unsigned char)ptr[1] == 0xBB) ptr += 3;
            
            cJSON *root = cJSON_Parse(ptr);
            if (root) {
                cJSON *ver_item = cJSON_GetObjectItem(root, app_id);
                if (ver_item && ver_item->valuestring) {
                    strncpy(out_version, ver_item->valuestring, max_len - 1);
                    out_version[max_len - 1] = '\0';
                }
                cJSON_Delete(root);
            }
            heap_caps_free(jstr);
        }
        fclose(vf);
    }
}

// ==========================================
// CÓDIGOS DA APP STORE & GITHUB ZERO-API
// ==========================================
static void update_local_version(const char* id, const char* version) {
    FILE *vf = fopen("/sdcard/apps/versions.json", "r");
    cJSON *root = NULL;
    if (vf) {
        fseek(vf, 0, SEEK_END);
        long fsize = ftell(vf);
        fseek(vf, 0, SEEK_SET);
        if (fsize > 0) {
            char *jstr = (char*)heap_caps_malloc(fsize + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            fread(jstr, 1, fsize, vf);
            char *ptr = jstr;
            if ((unsigned char)ptr[0] == 0xEF && (unsigned char)ptr[1] == 0xBB) ptr += 3;
            root = cJSON_Parse(ptr);
            heap_caps_free(jstr);
        }
        fclose(vf);
    }
    if (!root) root = cJSON_CreateObject();

    if (version == NULL) cJSON_DeleteItemFromObject(root, id);
    else {
        if (cJSON_HasObjectItem(root, id)) cJSON_ReplaceItemInObject(root, id, cJSON_CreateString(version));
        else cJSON_AddStringToObject(root, id, version);
    }

    char *new_jstr = cJSON_PrintUnformatted(root);
    vf = fopen("/sdcard/apps/versions.json", "w");
    if (vf) { fputs(new_jstr, vf); fclose(vf); }
    free(new_jstr);
    cJSON_Delete(root);
}

// O Wget Otimizado para baixar pacotes com redirecionamentos transparentes
static bool download_file(const char* url, const char* filepath, const char* msg) {
    ESP_LOGI(TAG, "=> Download Request: %s", url);
    
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_label_set_text(lbl_status, msg);
        bsp_display_unlock();
    }
    FILE *f = fopen(filepath, "wb");
    if (!f) return false;

    esp_http_client_config_t config = {};
    config.url = url;
    config.crt_bundle_attach = esp_crt_bundle_attach; 
    config.buffer_size_tx = 2048; 
    config.buffer_size = 16384; 
    config.user_data = f; 
    
    config.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        if (evt->event_id == HTTP_EVENT_ON_DATA && !esp_http_client_is_chunked_response(evt->client)) {
            FILE *fp = (FILE*)evt->user_data;
            fwrite(evt->data, 1, evt->data_len, fp);
        }
        return ESP_OK;
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_redirection(client); 
    esp_http_client_set_header(client, "User-Agent", "ESP32-Smartwatch-Updater");

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    fclose(f);
    esp_http_client_cleanup(client);

    // Se falhou OU se a página retornou "404 Not Found", deleta o arquivo inútil gerado!
    if (err != ESP_OK || status != 200) {
        remove(filepath); 
        ESP_LOGE(TAG, "Falha Download (HTTP %d): %s", status, url);
        return false;
    }
    return true;
}

static void process_jobs_task(void *pvParameters) {
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_set_hidden(overlay_loading, false); 
        bsp_display_unlock();
    }

    int total_jobs = cJSON_GetArraySize(job_queue);
    bool all_success = true;

    for (int i = 0; i < total_jobs; i++) {
        cJSON *job = cJSON_GetArrayItem(job_queue, i);
        const char *id = cJSON_GetObjectItem(job, "id")->valuestring;
        const char *version = cJSON_GetObjectItem(job, "version")->valuestring;
        const char *type = cJSON_GetObjectItem(job, "type")->valuestring;
        int action = cJSON_GetObjectItem(job, "action")->valueint; 

        if (action == 0) {
            // DESINSTALAÇÃO
            if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                lv_label_set_text_fmt(lbl_status, "Removendo %s...", id);
                bsp_display_unlock();
            }
            char path[128];
            snprintf(path, sizeof(path), "/sdcard/apps/%s/app.bin", id); remove(path);
            snprintf(path, sizeof(path), "/sdcard/apps/%s/app.json", id); remove(path);
            snprintf(path, sizeof(path), "/sdcard/apps/%s/icon.png", id); remove(path);
            snprintf(path, sizeof(path), "/sdcard/apps/%s", id); rmdir(path);
            update_local_version(id, NULL);

        } else if (action == 1) {
            // INSTALAÇÃO OU UPDATE (Com Proteção Atômica Contra Falhas)
            char url_bin[256], url_json[256], url_png[256];
            bool ok = false;

            if (strcmp(type, "sys") == 0) {
                // FACTORY
                snprintf(url_bin, sizeof(url_bin), "https://github.com/Lucas-D-Souza/app_%s/releases/download/%s/factory.bin", id, version);
                char msg[64]; snprintf(msg, sizeof(msg), "Baixando Firmware...\n(%d de %d)", i+1, total_jobs);
                
                // Limpa lixo antigo temporário se existir
                remove("/sdcard/factory.tmp");

                if (download_file(url_bin, "/sdcard/factory.tmp", msg)) {
                    remove("/sdcard/factory.bin"); // Só apaga o oficial DEPOIS do .tmp dar sucesso
                    rename("/sdcard/factory.tmp", "/sdcard/factory.bin");
                    ok = flash_factory_from_sd();
                } else {
                    remove("/sdcard/factory.tmp"); // Apaga o arquivo corrompido, mantendo o velho
                }
            } else {
                // APP PADRÃO
                char dir_path[64];
                snprintf(dir_path, sizeof(dir_path), "/sdcard/apps/%s", id);
                mkdir(dir_path, 0777); 

                // Prepara links da nuvem
                snprintf(url_bin, sizeof(url_bin), "https://github.com/Lucas-D-Souza/app_%s/releases/download/%s/app.bin", id, version);
                snprintf(url_json, sizeof(url_json), "https://github.com/Lucas-D-Souza/app_%s/releases/download/%s/app.json", id, version);
                snprintf(url_png, sizeof(url_png), "https://github.com/Lucas-D-Souza/app_%s/releases/download/%s/icon.png", id, version);

                // NOMES TEMPORÁRIOS (Proteção)
                char tmp_bin[128];  snprintf(tmp_bin,  sizeof(tmp_bin),  "%s/app.bin.tmp", dir_path);
                char tmp_json[128]; snprintf(tmp_json, sizeof(tmp_json), "%s/app.json.tmp", dir_path);
                char tmp_png[128];  snprintf(tmp_png,  sizeof(tmp_png),  "%s/icon.png.tmp", dir_path);

                // NOMES FINAIS
                char final_bin[128];  snprintf(final_bin,  sizeof(final_bin),  "%s/app.bin", dir_path);
                char final_json[128]; snprintf(final_json, sizeof(final_json), "%s/app.json", dir_path);
                char final_png[128];  snprintf(final_png,  sizeof(final_png),  "%s/icon.png", dir_path);

                char msg_bin[64]; snprintf(msg_bin, sizeof(msg_bin), "Baixando %s\n(%d de %d)", id, i+1, total_jobs);
                
                // Limpa possíveis lixos de downloads interrompidos anteriormente
                remove(tmp_bin); remove(tmp_json); remove(tmp_png);

                // TENTA BAIXAR TUDO PARA OS ARQUIVOS .TMP (Sem tocar nos arquivos que já rodam no relógio)
                ok = download_file(url_bin, tmp_bin, msg_bin);
                if (ok) ok = download_file(url_json, tmp_json, "Baixando metadados...");
                if (ok) ok = download_file(url_png, tmp_png, "Baixando icone...");

                if (ok) {
                    // SUCESSO ABSOLUTO! Agora sim apagamos as versões velhas e oficializamos as novas
                    remove(final_bin);  rename(tmp_bin, final_bin);
                    remove(final_json); rename(tmp_json, final_json);
                    remove(final_png);  rename(tmp_png, final_png);
                    ESP_LOGI(TAG, "Instalacao do %s finalizada com seguranca.", id);
                } else {
                    // FALHA! Apaga apenas o lixo .tmp. O app antigo não sofreu nenhum arranhão!
                    ESP_LOGE(TAG, "Download do %s falhou! Revertendo e mantendo versao anterior.", id);
                    remove(tmp_bin);
                    remove(tmp_json);
                    remove(tmp_png);
                }
            }

            if (ok) update_local_version(id, version);
            else all_success = false;
        }
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        if (all_success) {
            lv_label_set_text(lbl_status, "Concluido! " LV_SYMBOL_OK);
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00FF00), 0);
        } else {
            lv_label_set_text(lbl_status, "Falha na operacao!\n(Erro 404 ou Wi-Fi)");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF0000), 0);
        }
        bsp_display_unlock();
    }
    
    vTaskDelay(pdMS_TO_TICKS(2000)); 
    xTaskCreatePinnedToCore(load_catalog_task, "catalog", 16384, NULL, 5, NULL, 1);
    vTaskDelete(NULL);
}

static void btn_action_click_cb(lv_event_t * e) {
    cJSON *pkg = (cJSON*)lv_event_get_user_data(e);
    int action = cJSON_GetObjectItem(pkg, "action")->valueint;
    
    if (job_queue) cJSON_Delete(job_queue);
    job_queue = cJSON_CreateArray();

    if (action == 2) { 
        for(int i=0; i < cJSON_GetArraySize(pending_updates); i++) {
            cJSON_AddItemToArray(job_queue, cJSON_Duplicate(cJSON_GetArrayItem(pending_updates, i), 1));
        }
    } else {
        cJSON_AddItemToArray(job_queue, cJSON_Duplicate(pkg, 1));
    }
    xTaskCreatePinnedToCore(process_jobs_task, "jobs", 24000, NULL, 5, NULL, 1);
}

static void load_catalog_task(void *pvParameters) {
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_set_hidden(overlay_loading, false);
        lv_label_set_text(lbl_status, "Lendo Catalogo da Nuvem...");
        lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
        bsp_display_unlock();
    }
    
    const char * raw_url = "https://raw.githubusercontent.com/Lucas-D-Souza/app_store_catalog/main/catalog.json";
    if (!download_file(raw_url, "/sdcard/config/catalog.tmp", "Baixando Catalogo...")) {
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            lv_label_set_text(lbl_status, "Falha de Conexao!");
            bsp_display_unlock();
        }
        vTaskDelete(NULL);
        return;
    }

    FILE *cf = fopen("/sdcard/config/catalog.tmp", "r");
    fseek(cf, 0, SEEK_END);
    long csize = ftell(cf);
    fseek(cf, 0, SEEK_SET);
    char *cstr = (char*)heap_caps_malloc(csize + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    fread(cstr, 1, csize, cf);
    fclose(cf);
    cstr[csize] = 0;
    cJSON *catalog = cJSON_Parse(cstr);
    heap_caps_free(cstr);

    FILE *vf = fopen("/sdcard/apps/versions.json", "r");
    cJSON *local_versions = NULL;
    if (vf) {
        fseek(vf, 0, SEEK_END);
        long fsize = ftell(vf);
        fseek(vf, 0, SEEK_SET);
        char *jstr = (char*)heap_caps_malloc(fsize + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        fread(jstr, 1, fsize, vf);
        fclose(vf);
        jstr[fsize] = 0;
        char *ptr = jstr;
        if ((unsigned char)ptr[0] == 0xEF && (unsigned char)ptr[1] == 0xBB) ptr += 3;
        local_versions = cJSON_Parse(ptr);
        heap_caps_free(jstr);
    }
    if (!local_versions) local_versions = cJSON_CreateObject();

    if (pending_updates) cJSON_Delete(pending_updates);
    pending_updates = cJSON_CreateArray();
    
    if (ui_packages) cJSON_Delete(ui_packages);
    ui_packages = cJSON_CreateArray();

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_clean(tab_loja);
        lv_obj_clean(tab_updates);
        lv_obj_set_flex_flow(tab_loja, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_flow(tab_updates, LV_FLEX_FLOW_COLUMN);
        bsp_display_unlock();
    }

    int updates_count = 0;

    for (int i = 0; i < cJSON_GetArraySize(catalog); i++) {
        cJSON *cat_app = cJSON_GetArrayItem(catalog, i);
        const char *id = cJSON_GetObjectItem(cat_app, "id")->valuestring;
        const char *name = cJSON_GetObjectItem(cat_app, "name")->valuestring;
        const char *cat_version = cJSON_GetObjectItem(cat_app, "version")->valuestring;
        cJSON *type_item = cJSON_GetObjectItem(cat_app, "type");
        const char *type = (type_item && type_item->valuestring) ? type_item->valuestring : "app";

        cJSON *local_app = cJSON_GetObjectItem(local_versions, id);
        bool is_installed = (local_app != NULL);

        // --- LOG DE DEBUG PARA O MONITOR SERIAL ---
        if (is_installed) {
            ESP_LOGW(TAG, "App: %s | Local: v%s | Nuvem: v%s", id, local_app->valuestring, cat_version);
        } else {
            ESP_LOGW(TAG, "App: %s | Local: N/A | Nuvem: v%s", id, cat_version);
        }

        // VARIÁVEIS DE DESENHO DA TELA
        bool show_in_store = true; // Apps sempre aparecem na loja agora!
        bool show_in_update = false;
        const char * store_btn_sym = "";
        uint32_t store_btn_color = 0;
        int store_action = 0;

        if (is_installed) {
            const char *local_version = local_app->valuestring;
            
            // SE TIVER UPDATE, MOSTRA NA ABA UPDATE E DEIXA A LIXEIRA NA LOJA
            if (strcmp(local_version, cat_version) != 0) {
                show_in_update = true;
                
                cJSON *p = cJSON_CreateObject();
                cJSON_AddStringToObject(p, "id", id);
                cJSON_AddStringToObject(p, "version", cat_version);
                cJSON_AddStringToObject(p, "type", type);
                cJSON_AddNumberToObject(p, "action", 1);
                cJSON_AddItemToArray(pending_updates, p);
                
                if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                    lv_obj_t * row = lv_obj_create(tab_updates);
                    lv_obj_set_width(row, lv_pct(100));
                    lv_obj_set_height(row, LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
                    lv_obj_set_style_border_width(row, 0, 0);
                    
                    lv_obj_t * lbl_name = lv_label_create(row);
                    lv_label_set_text_fmt(lbl_name, "%s\nv%s -> %s", name, local_version, cat_version);
                    lv_obj_set_style_text_color(lbl_name, lv_color_white(), 0);
                    lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 0, 0);
                    
                    lv_obj_t * btn = lv_button_create(row);
                    lv_obj_set_size(btn, 60, 45);
                    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x007BFF), 0); // Azul
                    
                    lv_obj_t * lbl_btn = lv_label_create(btn);
                    lv_label_set_text(lbl_btn, LV_SYMBOL_DOWNLOAD);
                    lv_obj_center(lbl_btn);

                    cJSON *pkg = cJSON_Duplicate(p, 1);
                    cJSON_AddItemToArray(ui_packages, pkg);
                    lv_obj_add_event_cb(btn, btn_action_click_cb, LV_EVENT_CLICKED, pkg);
                    bsp_display_unlock();
                }
                updates_count++;
            } 
            
            // CONFIGURA A LINHA DA LOJA (Lixeira) - Se não for "sys" nem o próprio "updater"
            if (strcmp(type, "sys") != 0 && strcmp(id, "updater") != 0) {
                store_action = 0; // Excluir
                store_btn_sym = LV_SYMBOL_TRASH;
                store_btn_color = 0xDC3545; // Vermelho
            } else {
                show_in_store = false; // Factory e Updater ocultam os botões de Lixeira
                // Mostramos apenas o texto sem botão:
                if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                    lv_obj_t * row = lv_obj_create(tab_loja);
                    lv_obj_set_width(row, lv_pct(100));
                    lv_obj_set_height(row, LV_SIZE_CONTENT);
                    lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
                    lv_obj_set_style_border_width(row, 0, 0);
                    
                    lv_obj_t * lbl_name = lv_label_create(row);
                    lv_label_set_text_fmt(lbl_name, "%s\nInstalado (v%s)", name, local_version);
                    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0x888888), 0); // Cinza para travar
                    lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 0, 0);
                    bsp_display_unlock();
                }
            }

        } else {
            // NUNCA INSTALADO (Loja: Nuvem)
            store_action = 1; // Instalar
            store_btn_sym = LV_SYMBOL_UPLOAD;
            store_btn_color = 0x28A745; // Verde
        }

        // DESENHA A LINHA DA LOJA (Caso precise do Botão de Ação)
        if (show_in_store && bsp_display_lock(pdMS_TO_TICKS(100))) {
            lv_obj_t * row = lv_obj_create(tab_loja);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(row, lv_color_hex(0x222222), 0);
            lv_obj_set_style_border_width(row, 0, 0);
            
            lv_obj_t * lbl_name = lv_label_create(row);
            const char * ver_loja = is_installed ? local_app->valuestring : cat_version;
            lv_label_set_text_fmt(lbl_name, "%s\nv%s", name, ver_loja);
            lv_obj_set_style_text_color(lbl_name, lv_color_white(), 0);
            lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 0, 0);
            
            lv_obj_t * btn = lv_button_create(row);
            lv_obj_set_size(btn, 60, 45);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(store_btn_color), 0);
            
            lv_obj_t * lbl_btn = lv_label_create(btn);
            lv_label_set_text(lbl_btn, store_btn_sym);
            lv_obj_set_style_text_font(lbl_btn, &lv_font_montserrat_20, 0);
            lv_obj_center(lbl_btn);

            cJSON *pkg = cJSON_CreateObject();
            cJSON_AddStringToObject(pkg, "id", id);
            cJSON_AddStringToObject(pkg, "version", cat_version);
            cJSON_AddStringToObject(pkg, "type", type);
            cJSON_AddNumberToObject(pkg, "action", store_action);
            cJSON_AddItemToArray(ui_packages, pkg);

            lv_obj_add_event_cb(btn, btn_action_click_cb, LV_EVENT_CLICKED, pkg);
            bsp_display_unlock();
        }
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        if (updates_count > 1) {
            lv_obj_t * btn_all = lv_button_create(tab_updates);
            lv_obj_set_width(btn_all, lv_pct(100)); 
            lv_obj_set_style_bg_color(btn_all, lv_color_hex(0x0055A4), 0); 
            lv_obj_t * lbl_all = lv_label_create(btn_all);
            lv_label_set_text_fmt(lbl_all, "%s Atualizar Todos", LV_SYMBOL_DOWNLOAD);
            lv_obj_center(lbl_all); 
            
            cJSON *pkg_all = cJSON_CreateObject();
            cJSON_AddNumberToObject(pkg_all, "action", 2); 
            cJSON_AddItemToArray(ui_packages, pkg_all);

            lv_obj_add_event_cb(btn_all, btn_action_click_cb, LV_EVENT_CLICKED, pkg_all);
            lv_obj_move_to_index(btn_all, 0); 
        } else if (updates_count == 0) {
            lv_obj_t * lbl = lv_label_create(tab_updates);
            lv_label_set_text(lbl, "Tudo atualizado!");
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl, lv_pct(100)); 
        }

        lv_obj_set_hidden(overlay_loading, true);
        bsp_display_unlock();
    }

    cJSON_Delete(catalog);
    cJSON_Delete(local_versions);
    vTaskDelete(NULL);
}

// ==========================================
// TELA BASE E UI DO APP STORE
// ==========================================
static void build_store_ui() {
    scr_store = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_store, lv_color_black(), 0);
    lv_obj_set_scroll_dir(scr_store, LV_DIR_NONE);

    tv_store = lv_tabview_create(scr_store);
    lv_tabview_set_tab_bar_position(tv_store, LV_DIR_TOP);
    lv_tabview_set_tab_bar_size(tv_store, 50);
    lv_obj_set_style_bg_color(tv_store, lv_color_black(), 0);

    lv_obj_t * tab_bar = lv_tabview_get_tab_bar(tv_store);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_color(tab_bar, lv_color_white(), (lv_style_selector_t)LV_PART_ITEMS | (lv_style_selector_t)LV_STATE_CHECKED);

    tab_loja = lv_tabview_add_tab(tv_store, "Loja");
    tab_updates = lv_tabview_add_tab(tv_store, "Updates");

    lv_obj_set_style_pad_all(tab_loja, 5, 0);
    lv_obj_set_style_pad_all(tab_updates, 5, 0);

    overlay_loading = lv_obj_create(scr_store);
    lv_obj_set_size(overlay_loading, 340, 340);
    lv_obj_center(overlay_loading);
    lv_obj_set_style_bg_color(overlay_loading, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay_loading, LV_OPA_90, 0);
    lv_obj_set_style_border_width(overlay_loading, 0, 0);
    lv_obj_set_scroll_dir(overlay_loading, LV_DIR_NONE);
    lv_obj_set_hidden(overlay_loading, true); 

    spinner = lv_spinner_create(overlay_loading);
    lv_obj_set_size(spinner, 60, 60);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);

    lbl_status = lv_label_create(overlay_loading);
    lv_label_set_text(lbl_status, "Aguarde...");
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 20);
}

static void show_splash_screen(const char* version) {
    scr_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_splash, lv_color_black(), 0);
    lv_obj_set_scroll_dir(scr_splash, LV_DIR_NONE);

    lv_obj_t * cont_center = lv_obj_create(scr_splash);
    lv_obj_set_size(cont_center, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(cont_center, LV_ALIGN_CENTER, 0, -30);
    lv_obj_set_style_bg_opa(cont_center, LV_OPA_TRANSP, 0); 
    lv_obj_set_style_border_width(cont_center, 0, 0); 
    
    lv_obj_set_flex_flow(cont_center, LV_FLEX_FLOW_ROW); 
    lv_obj_set_flex_align(cont_center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(cont_center, 25, 0); 

    lv_obj_t * logo = lv_image_create(cont_center);
    lv_image_set_src(logo, &icon_updater);
    lv_image_set_scale(logo, 512); 
    lv_obj_set_size(logo, 100, 100);

    lv_obj_t * title = lv_label_create(cont_center);
    lv_label_set_text(title, "App\nStore"); 
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0); 
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lbl_splash_version = lv_label_create(scr_splash);
    lv_label_set_text_fmt(lbl_splash_version, "v%s", version);
    lv_obj_set_style_text_color(lbl_splash_version, lv_color_hex(0x555555), 0); 
    lv_obj_align(lbl_splash_version, LV_ALIGN_BOTTOM_MID, 0, -25);

    lv_screen_load(scr_splash);
}


// ==========================================
// INTERFACE: WI-FI LISTA & SENHA 
// ==========================================
static void start_wifi_scan(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) esp_wifi_disconnect(); 
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    scan_config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_wifi_scan_start(&scan_config, false);
}

static void wifi_list_item_click_cb(lv_event_t * e) {
    lv_obj_t * btn = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0); 
    if (label) {
        const char * text = lv_label_get_text(label);
        const char * paren = strrchr(text, '(');
        int len = (paren != NULL && paren > text) ? (paren - text - 1) : 32; 
        if (len > 32) len = 32;
        strncpy(current_ssid, text, len);
        current_ssid[len] = '\0';
        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "Senha: %s", current_ssid);
        lv_label_set_text(label_wifi_title, title_buf);
        lv_textarea_set_text(ta_wifi_pass, ""); 
        lv_scr_load_anim(scr_password, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    }
}

static void build_wifi_ui() {
    scr_wifi_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wifi_list, lv_color_black(), 0);
    
    lv_obj_t * header = lv_label_create(scr_wifi_list);
    lv_label_set_text(header, LV_SYMBOL_WIFI " Redes Wi-Fi"); 
    lv_obj_set_style_text_color(header, lv_color_white(), 0);
    lv_obj_set_style_text_font(header, &lv_font_montserrat_20, 0);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 30); 

    list_wifi = lv_obj_create(scr_wifi_list);
    lv_obj_set_size(list_wifi, 310, 380); 
    lv_obj_align(list_wifi, LV_ALIGN_TOP_MID, 0, 70); 
    lv_obj_set_style_bg_color(list_wifi, lv_color_black(), 0);
    lv_obj_set_style_border_width(list_wifi, 0, 0);
    lv_obj_set_flex_flow(list_wifi, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * txt = lv_label_create(list_wifi);
    lv_label_set_text(txt, "Buscando...");

    scr_password = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_password, lv_color_black(), 0);

    label_wifi_title = lv_label_create(scr_password);
    lv_obj_set_style_text_color(label_wifi_title, lv_color_white(), 0); 
    lv_obj_set_style_text_font(label_wifi_title, &lv_font_montserrat_20, 0);
    lv_label_set_text(label_wifi_title, "Senha:");
    lv_obj_align(label_wifi_title, LV_ALIGN_TOP_LEFT, 50, 30);

    ta_wifi_pass = lv_textarea_create(scr_password);
    lv_textarea_set_password_mode(ta_wifi_pass, false); 
    lv_textarea_set_one_line(ta_wifi_pass, true);
    lv_obj_set_width(ta_wifi_pass, 310);
    lv_obj_align(ta_wifi_pass, LV_ALIGN_TOP_MID, 0, 70); 
    style_dark_ta(ta_wifi_pass);

    lv_obj_t * kb = lv_keyboard_create(scr_password);
    lv_keyboard_set_textarea(kb, ta_wifi_pass);
    style_dark_kb(kb);
    
    lv_obj_add_event_cb(kb, [](lv_event_t * e) {
        lv_label_set_text(label_wifi_title, "Conectando...");
        wifi_config_t wifi_config = {};
        strncpy((char *)wifi_config.sta.ssid, current_ssid, 32);
        strncpy((char *)wifi_config.sta.password, lv_textarea_get_text(ta_wifi_pass), 64);
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_connect();
    }, LV_EVENT_READY, NULL);

    lv_obj_add_event_cb(kb, [](lv_event_t * e) {
        lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, false);
    }, LV_EVENT_CANCEL, NULL);
}

// ==========================================
// EVENTOS WI-FI INTELIGENTE
// ==========================================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        wifi_config_t saved_config = {};
        esp_wifi_get_config(WIFI_IF_STA, &saved_config);
        if (strlen((char*)saved_config.sta.ssid) > 0) {
            if (bsp_display_lock(portMAX_DELAY)) {
                lv_label_set_text_fmt(lbl_status, "Conectando a:\n%s", saved_config.sta.ssid);
                lv_scr_load_anim(scr_store, LV_SCR_LOAD_ANIM_FADE_ON, 400, 2000, true);
                lv_obj_set_hidden(overlay_loading, false); 
                bsp_display_unlock();
            }
            esp_wifi_connect();
        } else {
            start_wifi_scan();
            if (bsp_display_lock(portMAX_DELAY)) {
                lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_FADE_ON, 400, 2000, true);
                bsp_display_unlock();
            }
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        start_wifi_scan();
        if (bsp_display_lock(portMAX_DELAY)) {
            if (lv_screen_active() != scr_wifi_list) {
                lv_scr_load_anim(scr_wifi_list, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
            }
            bsp_display_unlock();
        }
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 15) ap_count = 15;
        
        wifi_ap_record_t *ap_info = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * ap_count);
        if (ap_info && esp_wifi_scan_get_ap_records(&ap_count, ap_info) == ESP_OK) {
            if (bsp_display_lock(portMAX_DELAY)) {
                lv_obj_clean(list_wifi); 
                for (int i = 0; i < ap_count; i++) {
                    char ssid_str[33];
                    strncpy(ssid_str, (char *)ap_info[i].ssid, 32);
                    ssid_str[32] = '\0';
                    if (strlen(ssid_str) == 0) continue;

                    char list_item_text[64];
                    snprintf(list_item_text, sizeof(list_item_text), "%s (%d dBm)", ssid_str, ap_info[i].rssi);

                    lv_obj_t * btn = lv_button_create(list_wifi);
                    lv_obj_set_width(btn, lv_pct(100));
                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
                    lv_obj_t * lbl = lv_label_create(btn);
                    lv_label_set_text(lbl, list_item_text);
                    lv_obj_center(lbl);
                    lv_obj_add_event_cb(btn, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
                }
                bsp_display_unlock();
            }
        }
        if(ap_info) free(ap_info);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (bsp_display_lock(portMAX_DELAY)) {
            if (lv_screen_active() != scr_store) lv_scr_load_anim(scr_store, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
            bsp_display_unlock();
        }
        xTaskCreatePinnedToCore(load_catalog_task, "catalog", 16384, NULL, 5, NULL, 1);
    }
}

// ==========================================
// INTEGRIDADE DO SISTEMA DE ARQUIVOS
// ==========================================
static void ensure_fs_structure() {
    // 1. Garante que as pastas vitais existem
    mkdir("/sdcard/config", 0777);
    mkdir("/sdcard/apps", 0777);

    // 2. Verifica se o banco de dados de versão sumiu/não existe
    FILE *f = fopen("/sdcard/apps/versions.json", "r");
    if (f) {
        fclose(f); // O arquivo existe, tudo perfeito!
    } else {
        ESP_LOGW(TAG, "versions.json ausente! Varrendo a pasta /apps para recriar...");
        cJSON *root = cJSON_CreateObject();
        
        DIR *dir = opendir("/sdcard/apps");
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                // Ignora caminhos relativos e o próprio arquivo json (caso exista lixo)
                if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0 && strcmp(ent->d_name, "versions.json") != 0) {
                    
                    // Confirma se o que achamos é realmente uma pasta (e não um arquivo solto)
                    char path[300];
                    snprintf(path, sizeof(path), "/sdcard/apps/%s", ent->d_name);
                    struct stat st;
                    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                        // Pasta encontrada! Adiciona ao JSON com a versão 0.0.0
                        cJSON_AddStringToObject(root, ent->d_name, "0.0.0");
                        ESP_LOGI(TAG, "App '%s' detectado e registrado como v0.0.0", ent->d_name);
                    }
                }
            }
            closedir(dir);
        }

        // Caso tenha o factory na raiz do SD, adiciona também
        struct stat st_fact;
        if (stat("/sdcard/factory.bin", &st_fact) == 0) {
            cJSON_AddStringToObject(root, "factory", "0.0.0");
            ESP_LOGI(TAG, "Factory.bin detectado na raiz.");
        }
        
        // Salva o arquivo reconstruído
        char *json_str = cJSON_PrintUnformatted(root);
        FILE *vf = fopen("/sdcard/apps/versions.json", "w");
        if (vf) {
            fputs(json_str, vf);
            fclose(vf);
        }
        free(json_str);
        cJSON_Delete(root);
    }
}

// ==========================================
// INICIALIZAÇÃO PRINCIPAL
// ==========================================
extern "C" void app_main(void) {
    clear_i2c_bus();
    esp_ota_mark_app_valid_cancel_rollback();

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << BOOT_BTN_PIN);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    bsp_display_start();
    
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        show_splash_screen("...");
        bsp_display_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_display_brightness_set(80);

    SdUsbManager::get_instance().init_local_storage();

    ensure_fs_structure();

    // Lê a versão do JSON e atualiza a tela na mesma hora
    char current_ver[16];
    get_app_version_from_sd("updater", current_ver, sizeof(current_ver));
    
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        if(lbl_splash_version) {
            lv_label_set_text_fmt(lbl_splash_version, "v%s", current_ver);
        }
        bsp_display_unlock();
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        build_wifi_ui();
        build_store_ui();
        bsp_display_unlock();
    }

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    esp_wifi_set_storage(WIFI_STORAGE_FLASH); 
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    while(1) {
        if (gpio_get_level(BOOT_BTN_PIN) == 0) return_to_factory();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}