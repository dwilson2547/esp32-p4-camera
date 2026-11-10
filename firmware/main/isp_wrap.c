// Let a bypassed ISP pass frames wider than 1920 px.
//
// esp_isp_new_processor() rejects h_res > ISP_LL_HSIZE_MAX (1920) before it
// looks at bypass_isp. With the ISP bypassed nothing is processed: only the
// input module runs, and its line length register then counts 32-bit words,
// not pixels (isp_core.c sets it to ceil(h_res * bpp / 32)). A 4056-px RAW10
// line is 1268 words, inside the 12-bit field. So for a bypassed processor
// wider than the limit, create it at the limit and then write the real line
// length.
//
// Linked with -Wl,--wrap=esp_isp_new_processor (main/CMakeLists.txt), so
// esp_video's call lands here without patching esp_video or ESP-IDF.
#include "esp_log.h"
#include "driver/isp_core.h"
#include "hal/isp_ll.h"
#include "hal/color_hal.h"
#include "soc/isp_struct.h"

esp_err_t __real_esp_isp_new_processor(const esp_isp_processor_cfg_t *cfg, isp_proc_handle_t *ret);

esp_err_t __wrap_esp_isp_new_processor(const esp_isp_processor_cfg_t *cfg, isp_proc_handle_t *ret)
{
    if (!cfg || !cfg->flags.bypass_isp || cfg->h_res <= ISP_LL_HSIZE_MAX) {
        return __real_esp_isp_new_processor(cfg, ret);
    }
    esp_isp_processor_cfg_t c = *cfg;
    c.h_res = ISP_LL_HSIZE_MAX;
    esp_err_t e = __real_esp_isp_new_processor(&c, ret);
    if (e == ESP_OK) {
        uint32_t bpp = color_hal_pixel_format_fourcc_get_bit_depth(cfg->input_data_color_type);
        uint32_t words = (cfg->h_res * bpp + 31) / 32;
        isp_ll_set_intput_data_h_pixel_num(&ISP, words);
        ESP_LOGI("isp_wrap", "bypassed ISP at %" PRIu32 "x%" PRIu32 ": line length %" PRIu32 " words",
                 cfg->h_res, cfg->v_res, words);
    }
    return e;
}
