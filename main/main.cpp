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

static const char *TAG = "Updater";
#define BOOT_BTN_PIN GPIO_NUM_0

// Telas e Variáveis Globais
static lv_obj_t * scr_splash = NULL;
static lv_obj_t * scr_wifi_list = NULL;
static lv_obj_t * scr_password = NULL;
static lv_obj_t * scr_updater = NULL;

static lv_obj_t * list_wifi = NULL;
static lv_obj_t * ta_wifi_pass = NULL;
static lv_obj_t * label_wifi_title = NULL;
static char current_ssid[33] = {0};

static lv_obj_t * lbl_status = NULL;
static lv_obj_t * spinner = NULL;
static lv_obj_t * list_updates = NULL;

// Declaração do ícone compilado junto ao binário
LV_IMAGE_DECLARE(icon_updater); 

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

static void build_updater_ui() {
    scr_updater = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_updater, lv_color_black(), 0);
    lv_obj_set_scroll_dir(scr_updater, LV_DIR_NONE);

    spinner = lv_spinner_create(scr_updater);
    lv_obj_set_size(spinner, 80, 80);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x007BFF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 8, LV_PART_INDICATOR);

    lbl_status = lv_label_create(scr_updater);
    lv_label_set_text(lbl_status, "Iniciando App Store...");
    lv_obj_set_style_text_color(lbl_status, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_status, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(lbl_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_status, LV_ALIGN_CENTER, 0, 40);
}

// ==========================================
// FUNÇÕES DE HARDWARE
// ==========================================
static void return_to_factory() {
    ESP_LOGI(TAG, "Retornando ao Factory Firmware...");
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        bsp_display_brightness_set(0); 
        bsp_display_unlock();
    }
    const esp_partition_t *factory_part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory_part) {
        esp_ota_set_boot_partition(factory_part);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        esp_restart();
    }
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

// ==========================================
// CÓDIGOS DA APP STORE & GITHUB
// ==========================================
static void download_and_install_task(void *pvParameters) {
    cJSON *app_info = (cJSON*)pvParameters;
    const char* app_id = cJSON_GetObjectItem(app_info, "id")->valuestring;
    const char* download_url = cJSON_GetObjectItem(app_info, "url")->valuestring;
    const char* new_version = cJSON_GetObjectItem(app_info, "version")->valuestring;
    
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_clean(scr_updater); 
        build_updater_ui(); 
        lv_obj_set_hidden(spinner, false);
        lv_label_set_text_fmt(lbl_status, "Baixando %s...\nPor favor aguarde.", app_id);
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "Iniciando Download: %s", download_url);

    // --- NOVA ABORDAGEM DE DOWNLOAD: USO DO EVENT HANDLER ---
    
    char tmp_path[128];
    char final_path[128];
    
    if (strcmp(app_id, "factory") == 0) {
        snprintf(tmp_path, sizeof(tmp_path), "/sdcard/factory.tmp");
        snprintf(final_path, sizeof(final_path), "/sdcard/factory.bin");
    } else {
        snprintf(tmp_path, sizeof(tmp_path), "/sdcard/apps/%s/app.tmp", app_id);
        snprintf(final_path, sizeof(final_path), "/sdcard/apps/%s/app.bin", app_id);
    }

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Falha ao criar arquivo %s", tmp_path);
        vTaskDelete(NULL);
        return;
    }

    // Variáveis de escopo local para monitorar o andamento
    int total_bytes = 0;
    bool download_success = false;

    // Criamos um callback engenhoso que o HTTP_CLIENT vai chamar toda vez que chegar um "pedaço" do arquivo
    esp_http_client_config_t config = {};
    config.url = download_url;
    config.crt_bundle_attach = esp_crt_bundle_attach; 
    config.buffer_size_tx = 2048; 
    config.buffer_size = 16384; // Chunk seguro para a PSRAM
    config.user_data = f; // Passamos o arquivo SD direto para o Callback
    config.event_handler = [](esp_http_client_event_t *evt) -> esp_err_t {
        switch (evt->event_id) {
            case HTTP_EVENT_ON_HEADER:
                // Tenta pescar o Content-Length do Header verdadeiro da AWS, ignorando o do GitHub
                if (strcasecmp(evt->header_key, "Content-Length") == 0) {
                    // Nós colocaremos o total_bytes numa variável estática atrelada a task
                }
                break;
            case HTTP_EVENT_ON_DATA:
                if (!esp_http_client_is_chunked_response(evt->client)) {
                    FILE *fp = (FILE*)evt->user_data;
                    fwrite(evt->data, 1, evt->data_len, fp);
                    // O esp_http_client_perform gerencia as travas de hardware automaticamente
                }
                break;
            default:
                break;
        }
        return ESP_OK;
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_redirection(client); 
    
    // Header vital! O GitHub recusa imediatamente sem ele.
    esp_http_client_set_header(client, "User-Agent", "ESP32-Smartwatch-Updater");

    // A MÁGICA: esp_http_client_perform executa TODO o ciclo (Redirecionamentos, Headers e Payload) sozinho!
    ESP_LOGI(TAG, "Iniciando a sessao perform (Lidando com Redirecionamentos AWS automaticamente)...");
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        total_bytes = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTPS Status = %d, Arquivo recebido: %d bytes", status_code, total_bytes);
        
        if (status_code == 200) {
            download_success = true;
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request falhou: %s", esp_err_to_name(err));
    }

    fclose(f);
    esp_http_client_cleanup(client);

    // ==========================================
    // ETAPA DE CONSOLIDAÇÃO DO ARQUIVO
    // ==========================================
    if (download_success) {
        ESP_LOGI(TAG, "Download Perfeito. Convertendo TMP em BIN...");
        
        remove(final_path);
        rename(tmp_path, final_path);

        FILE *vf = fopen("/sdcard/apps/versions.json", "r");
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

            cJSON *root = cJSON_Parse(ptr);
            heap_caps_free(jstr);

            if (root) {
                cJSON_ReplaceItemInObject(root, app_id, cJSON_CreateString(new_version));
                char *new_jstr = cJSON_PrintUnformatted(root);
                
                vf = fopen("/sdcard/apps/versions.json", "w");
                if (vf) {
                    fputs(new_jstr, vf);
                    fclose(vf);
                }
                free(new_jstr);
                cJSON_Delete(root);
            }
        }

        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            lv_obj_set_hidden(spinner, true);
            lv_label_set_text(lbl_status, "Atualizacao Concluida!\nPressione Botao Lateral.");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0x00FF00), 0);
            bsp_display_unlock();
        }
    } else {
        ESP_LOGE(TAG, "Download Corrompido ou Falhou. Abortando.");
        remove(tmp_path);
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            lv_obj_set_hidden(spinner, true);
            lv_label_set_text(lbl_status, "Falha no Download!\nVerifique o Wi-Fi.");
            lv_obj_set_style_text_color(lbl_status, lv_color_hex(0xFF0000), 0);
            bsp_display_unlock();
        }
    }

    cJSON_Delete(app_info); 
    vTaskDelete(NULL);
}

static void btn_update_click_cb(lv_event_t * e) {
    cJSON *app_info = (cJSON*)lv_event_get_user_data(e);
    xTaskCreatePinnedToCore(download_and_install_task, "dl_task", 16384, app_info, 5, NULL, 1);
}

static void check_updates_task(void *pvParameters) {
    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_label_set_text(lbl_status, "Conectado!\nLendo Cartao SD...");
        bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(500));

    cJSON *local_versions = NULL;
    FILE *vf = fopen("/sdcard/apps/versions.json", "r");
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

    if (!local_versions) {
        ESP_LOGE(TAG, "Falha ao ler versions.json local!");
        vTaskDelete(NULL);
        return;
    }

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        lv_obj_clean(scr_updater); 
        
        lv_obj_t * title = lv_label_create(scr_updater);
        lv_label_set_text(title, "Atualizacoes Disponiveis");
        lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

        list_updates = lv_obj_create(scr_updater); // Substituto do lv_list
        lv_obj_set_size(list_updates, 320, 370);
        lv_obj_align(list_updates, LV_ALIGN_BOTTOM_MID, 0, -20);
        lv_obj_set_style_bg_color(list_updates, lv_color_black(), 0);
        lv_obj_set_style_border_width(list_updates, 0, 0);
        lv_obj_set_flex_flow(list_updates, LV_FLEX_FLOW_COLUMN); // Organiza em lista
        
        bsp_display_unlock();
    }

    int updates_found = 0;
    cJSON *app_node = local_versions->child;
    
    esp_http_client_config_t config = {};
    config.crt_bundle_attach = esp_crt_bundle_attach; 
    config.buffer_size_tx = 1024;
    config.buffer_size = 8192; 

    while (app_node) {
        const char *app_id = app_node->string; 
        const char *current_ver = app_node->valuestring;
        
        char github_api_url[256];
        // SEUS REPOS DEVEM CHAMAR "app_doom", "app_gbc", etc. SE NÃO, ALTERE AQUI PARA %s
        snprintf(github_api_url, sizeof(github_api_url), "https://api.github.com/repos/Lucas-D-Souza/app_%s/releases/latest", app_id);
        
        config.url = github_api_url;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_http_client_set_header(client, "User-Agent", "ESP32-Updater");
        
        ESP_LOGI(TAG, "Checando %s...", github_api_url);

        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            
            char *resp_buf = (char*)heap_caps_calloc(1, 16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            int total_read = 0;
            while(1) {
                int read = esp_http_client_read(client, resp_buf + total_read, 16384 - total_read - 1);
                if (read <= 0) break;
                total_read += read;
            }
            
            if (total_read > 0) {
                cJSON *git_json = cJSON_Parse(resp_buf);
                if (git_json) {
                    cJSON *tag_item = cJSON_GetObjectItem(git_json, "tag_name");
                    cJSON *assets = cJSON_GetObjectItem(git_json, "assets");
                    
                    if (tag_item && tag_item->valuestring && assets && cJSON_GetArraySize(assets) > 0) {
                        const char *latest_ver = tag_item->valuestring;
                        
                        if (strcmp(latest_ver, current_ver) != 0) {
                            updates_found++;
                            
                            cJSON *first_asset = cJSON_GetArrayItem(assets, 0);
                            cJSON *dl_url = cJSON_GetObjectItem(first_asset, "browser_download_url");
                            
                            if (dl_url && dl_url->valuestring) {
                                cJSON *package = cJSON_CreateObject();
                                cJSON_AddStringToObject(package, "id", app_id);
                                cJSON_AddStringToObject(package, "version", latest_ver);
                                cJSON_AddStringToObject(package, "url", dl_url->valuestring);
                                
                                if (bsp_display_lock(pdMS_TO_TICKS(100))) {
                                    char btn_text[64];
                                    snprintf(btn_text, sizeof(btn_text), "%s  %s (v%s -> %s)", LV_SYMBOL_DOWNLOAD, app_id, current_ver, latest_ver);
                                    
                                    lv_obj_t * btn = lv_button_create(list_updates);
                                    lv_obj_set_width(btn, lv_pct(100)); // Ocupa a largura toda
                                    lv_obj_set_style_bg_color(btn, lv_color_hex(0x222222), 0);
                                    lv_obj_set_style_border_width(btn, 0, 0);
                                    lv_obj_set_style_pad_all(btn, 15, 0);
                                    
                                    lv_obj_t * lbl = lv_label_create(btn);
                                    lv_label_set_text(lbl, btn_text);
                                    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
                                    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
                                    lv_obj_center(lbl); // Centraliza texto e ícone no botão
                                    
                                    lv_obj_add_event_cb(btn, btn_update_click_cb, LV_EVENT_CLICKED, package);
                                    bsp_display_unlock();
                                }
                            }
                        }
                    }
                    cJSON_Delete(git_json);
                }
            }
            heap_caps_free(resp_buf);
        }
        esp_http_client_cleanup(client);
        app_node = app_node->next; 
        vTaskDelay(pdMS_TO_TICKS(200)); 
    }

    cJSON_Delete(local_versions);

    if (updates_found == 0) {
        if (bsp_display_lock(pdMS_TO_TICKS(100))) {
            lv_obj_t * lbl = lv_label_create(list_updates);
            lv_label_set_text(lbl, "Voce esta 100% atualizado! " LV_SYMBOL_OK);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF00), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(lbl, lv_pct(100)); // Centraliza no flex
            bsp_display_unlock();
        }
    }

    vTaskDelete(NULL);
}


// ==========================================
// SPLASH SCREEN
// ==========================================
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
    lv_obj_set_style_pad_all(cont_center, 0, 0);
    lv_obj_set_style_pad_column(cont_center, 25, 0); 

    lv_obj_t * logo = lv_image_create(cont_center);
    lv_image_set_src(logo, &icon_updater);
    lv_image_set_scale(logo, 512); 
    lv_obj_set_size(logo, 100, 100);

    lv_obj_t * title = lv_label_create(cont_center);
    lv_label_set_text(title, "Loja e\nAtualizacoes"); 
    lv_obj_set_style_text_font(title, &lv_font_montserrat_30, 0); 
    lv_obj_set_style_text_color(title, lv_color_white(), 0);

    lv_obj_t * lbl_version = lv_label_create(scr_splash);
    lv_label_set_text_fmt(lbl_version, "v%s", version);
    lv_obj_set_style_text_font(lbl_version, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_version, lv_color_hex(0x555555), 0); 
    lv_obj_align(lbl_version, LV_ALIGN_BOTTOM_MID, 0, -25);

    lv_screen_load(scr_splash);
}

// ==========================================
// INTERFACE: WI-FI LISTA & SENHA 
// ==========================================
static void start_wifi_scan(void) {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        esp_wifi_disconnect(); 
    }
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
    lv_obj_set_style_text_color(txt, lv_color_white(), 0);

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
                lv_scr_load_anim(scr_updater, LV_SCR_LOAD_ANIM_FADE_ON, 400, 2000, true);
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
                    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
                    lv_obj_center(lbl);
                    lv_obj_add_event_cb(btn, wifi_list_item_click_cb, LV_EVENT_CLICKED, NULL);
                }
                bsp_display_unlock();
            }
        }
        if(ap_info) free(ap_info);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "IP Local: " IPSTR, IP2STR(&event->ip_info.ip));
        
        if (bsp_display_lock(portMAX_DELAY)) {
            if (lv_screen_active() != scr_updater) {
                lv_scr_load_anim(scr_updater, LV_SCR_LOAD_ANIM_FADE_ON, 400, 0, false);
            }
            bsp_display_unlock();
        }
        
        xTaskCreatePinnedToCore(check_updates_task, "update_task", 16384, NULL, 5, NULL, 1);
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
        show_splash_screen("1.0.0");
        bsp_display_unlock();
    }

    vTaskDelay(pdMS_TO_TICKS(50));
    bsp_display_brightness_set(80);

    SdUsbManager::get_instance().init_local_storage();

    if (bsp_display_lock(pdMS_TO_TICKS(100))) {
        build_wifi_ui();
        build_updater_ui();
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