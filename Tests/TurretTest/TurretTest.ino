#include <ArduinoWebsockets.h>
#include "esp_http_server.h"
#include <WiFi.h>
#include "camera_index.h"
#include "esp_camera.h"
#include "fb_gfx.h"
#include "fd_forward.h"


const char* ssid = "NSA";
const char* password = "Orange";

// Arduino like analogWrite
// value has to be between 0 and valueMax
void ledcAnalogWrite(uint8_t channel, uint32_t value, uint32_t valueMax = 180)
{
  // calculate duty, 8191 from 2 ^ 13 - 1
  uint32_t duty = (8191 / valueMax) * min(value, valueMax);
  ledcWrite(channel, duty);
}
int pan_center = 90; // center the pan servo
int tilt_center = 90; // center the tilt servo


#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#define FACE_COLOR_GREEN  0x0000FF00

using namespace websockets;
WebsocketsServer socket_server;

static inline mtmn_config_t app_mtmn_config()
{
  mtmn_config_t mtmn_config = {0};
  mtmn_config.type = FAST;
  mtmn_config.min_face = 80;
  mtmn_config.pyramid = 0.707;
  mtmn_config.pyramid_times = 4;
  mtmn_config.p_threshold.score = 0.6;
  mtmn_config.p_threshold.nms = 0.7;
  mtmn_config.p_threshold.candidate_number = 20;
  mtmn_config.r_threshold.score = 0.7;
  mtmn_config.r_threshold.nms = 0.7;
  mtmn_config.r_threshold.candidate_number = 10;
  mtmn_config.o_threshold.score = 0.7;
  mtmn_config.o_threshold.nms = 0.7;
  mtmn_config.o_threshold.candidate_number = 1;
  return mtmn_config;
}
mtmn_config_t mtmn_config = app_mtmn_config();

httpd_handle_t camera_httpd = NULL;

void setup()
{
  Serial.begin(115200);
  Serial.println();

  // Ai-Thinker: pins 2 and 12
  ledcSetup(2, 50, 16); //channel, freq, resolution
  ledcAttachPin(2, 2); // pin, channel

  ledcSetup(4, 50, 16);
  ledcAttachPin(12, 4);

  ledcAnalogWrite(2, 90); // channel, 0-180
  delay(1000);
  ledcAnalogWrite(4, 90);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if (psramFound()) {
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }
#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif
  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);

  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  app_httpserver_init();
  socket_server.listen(82);

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
}

static esp_err_t index_handler(httpd_req_t *req)
{
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, (const char *)index_ov2640_html_gz, index_ov2640_html_gz_len);
}

httpd_uri_t index_uri = {
  .uri       = "/",
  .method    = HTTP_GET,
  .handler   = index_handler,
  .user_ctx  = NULL
};

void app_httpserver_init ()
{
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  if (httpd_start(&camera_httpd, &config) == ESP_OK)
    Serial.println("httpd_start");
  {
    httpd_register_uri_handler(camera_httpd, &index_uri);
  }
}

static void draw_face_boxes(dl_matrix3du_t *image_matrix, box_array_t *boxes)
{
  int x, y, w, h, i, half_width, half_height;
  uint32_t color = FACE_COLOR_GREEN;
  fb_data_t fb;
  fb.width = image_matrix->w;
  fb.height = image_matrix->h;
  fb.data = image_matrix->item;
  fb.bytes_per_pixel = 3;
  fb.format = FB_BGR888;
  for (i = 0; i < boxes->len; i++) {

    // Convoluted way of finding face centre...
    x = ((int)boxes->box[i].box_p[0]);
    w = (int)boxes->box[i].box_p[2] - x + 1;
    half_width = w / 2;
    int face_center_pan = x + half_width; // image frame face centre x co-ordinate

    y = (int)boxes->box[i].box_p[1];
    h = (int)boxes->box[i].box_p[3] - y + 1;
    half_height = h / 2;
    int face_center_tilt = y + half_height;  // image frame face centre y co-ordinate

    //    assume QVGA 320x240
    //    int sensor_width = 320;
    //    int sensor_height = 240;
    //    int lens_fov = 45
    //    float diagonal = sqrt(sq(sensor_width) + sq(sensor_height)); // pixels along the diagonal
    //    float pixels_per_degree = diagonal / lens_fov;
    //    400/45 = 8.89

    float move_to_x = pan_center + ((-160 + face_center_pan) / 8.89) ;
    float move_to_y = tilt_center + ((-120 + face_center_tilt) / 8.89) ;

    pan_center = (pan_center + move_to_x) / 2;
    Serial.println(pan_center);
    ledcAnalogWrite(2, pan_center); // channel, 0-180

    tilt_center = (tilt_center + move_to_y) / 2;
    int reversed_tilt_center = map(tilt_center, 0, 180, 180, 0);
    ledcAnalogWrite(4, reversed_tilt_center); // channel, 0-180

    fb_gfx_drawFastHLine(&fb, x, y, w, color);
    fb_gfx_drawFastHLine(&fb, x, y + h - 1, w, color);
    fb_gfx_drawFastVLine(&fb, x, y, h, color);
    fb_gfx_drawFastVLine(&fb, x + w - 1, y, h, color);
  }
}

void loop()
{
  auto client = socket_server.accept();
  camera_fb_t * fb = NULL;
  dl_matrix3du_t *image_matrix = NULL;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  
  while (true) {
    client.poll();
    fb = esp_camera_fb_get();
    _jpg_buf_len = fb->len;
    _jpg_buf = fb->buf;
    image_matrix = dl_matrix3du_alloc(1, fb->width, fb->height, 3);

    fmt2rgb888(fb->buf, fb->len, fb->format, image_matrix->item);

    box_array_t *net_boxes = NULL;
    net_boxes = face_detect(image_matrix, &mtmn_config);

    if (net_boxes) {
      draw_face_boxes(image_matrix, net_boxes);
      free(net_boxes->score);
      free(net_boxes->box);
      free(net_boxes->landmark);
      free(net_boxes);
    }
    fmt2jpg(image_matrix->item, fb->width * fb->height * 3, fb->width, fb->height, PIXFORMAT_RGB888, 90, &_jpg_buf, &_jpg_buf_len);
    client.sendBinary((const char *)_jpg_buf, _jpg_buf_len);

    esp_camera_fb_return(fb);
    fb = NULL;
    dl_matrix3du_free(image_matrix);
    free(_jpg_buf);
    _jpg_buf = NULL;

  }
}
