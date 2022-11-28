#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
 
#define WIN_WIDTH 1920
#define WIN_HEIGHT 1080
#define INBUF_SIZE 4096
#define BUFFERSIZE 10
#define FPS 24
#define FRAMES 600

static AVCodecContext *pCodecCtx= NULL;
static AVFormatContext *pFormatCtx = NULL;
static int width, height, video_dst_linesize[4], video_dst_bufsize;
static int count =0;
static enum AVPixelFormat pix_fmt;
static uint8_t *video_dst_data[4] = {NULL};
static AVFrame *srcFrame;
static AVFrame *consumerFrame = NULL;
static AVFrame *avFrameArr[10] = {NULL};
GtkWidget *darea;

static int frameIndex =0;

typedef struct circular_buf_t circular_buf_t;

typedef circular_buf_t *cbuf_handle_t;

struct circular_buf_t
{
  AVFrame *bufferFrames[10];
  size_t head;
  size_t tail;
  size_t max;
  bool full;
};

pthread_cond_t c;
pthread_cond_t d;
static cbuf_handle_t glob_buffer;
static cbuf_handle_t consumer_buff;

/* Circular buffer function declarations */
cbuf_handle_t circular_buf_init();

void circular_buf_reset(cbuf_handle_t me);

void circular_buf_free(cbuf_handle_t me);

bool circ_buf_full(cbuf_handle_t me);

bool circ_buf_empty(cbuf_handle_t me);

size_t circular_buf_capacity(cbuf_handle_t me);

size_t circular_buf_get_size(cbuf_handle_t me);

static void advance_pointer(cbuf_handle_t me);

static void retreat_pointer(cbuf_handle_t me);

int circular_buf_put(cbuf_handle_t me, AVFrame *data);

int circular_buf_get(cbuf_handle_t me, AVFrame *data);

/*Created threads function declarations*/

pthread_mutex_t lock;


static AVFrame* allocPicture(enum AVPixelFormat pix_fmt, int width, int height)
{
    // Allocate a frame
    AVFrame* frame = av_frame_alloc();

    if (frame == NULL)
    {
        fprintf(stderr, "avcodec_alloc_frame failed\n");
    }
  
    if (av_image_alloc(frame->data, frame->linesize, width, height, pix_fmt, 1) < 0)
    {
        fprintf(stderr, "av_image_alloc failed\n");
    }

    frame->width = width;
    frame->height = height;
    frame->format = pix_fmt;

    return frame;
}

/*
  Initialize a circular buffer handler
*/
cbuf_handle_t circular_buf_init(int size)
{
  cbuf_handle_t cbuf = malloc(sizeof(circular_buf_t));

  for(int i=0; i<size; i++) {
    cbuf->bufferFrames[i] = allocPicture(AV_PIX_FMT_RGB24, WIN_WIDTH, WIN_HEIGHT);
    if (!cbuf->bufferFrames[i]) {
      fprintf(stderr, "Could not allocate video frame within buffer Initialization\n");
      exit(1);
    }
  }
  cbuf->max = size;
  circular_buf_reset(cbuf);

  return cbuf;
}

void buffer_init() {
  for(int i=0; i<10; i++) {
    avFrameArr[i] = allocPicture(AV_PIX_FMT_RGB24, WIN_WIDTH, WIN_HEIGHT);
  }
}

/*
  Set header and tail to initial position and full FLAG to false
*/
void circular_buf_reset(cbuf_handle_t me)
{
  assert(me);

  me->head = 0;
  me->tail = 0;
  me->full = false;
}

/*
  Remove allocated circular buffer memory
*/
void circular_buf_free(cbuf_handle_t me)
{
  assert(me);
  free(me);
}

/*
  Check if circular buffer is full
*/
bool circ_buf_full(cbuf_handle_t me)
{
  assert(me);
  return me->full;
}

/*
  Check if circular buffer is empty
*/
bool circ_buf_empty(cbuf_handle_t me)
{
  assert(me);
  return (!me->full && (me->head == me->tail));
}

/*
  returns max capacity of circular buffer
*/
size_t circular_buf_capacity(cbuf_handle_t me)
{
  assert(me);

  return me->max;
}

/*
  returns the size of the circular buffer
*/
size_t circular_buf_get_size(cbuf_handle_t me)
{
  assert(me);
  size_t size = me->max;

  if (!me->full)
  {
    if (me->head >= me->tail)
    {
      size = (me->head - me->tail);
    }
    else
    {
      size = (me->max + me->head - me->tail);
    }
  }

  return size;
}

static void advance_pointer(cbuf_handle_t me)
{
  assert(me);

  if (me->full)
  {
    if (++(me->tail) == me->max)
    {
      me->tail = 0;
    }
  }

  if (++(me->head) == me->max)
  {
    me->head = 0;
  }
  me->full = (me->head == me->tail);
}

static void retreat_pointer(cbuf_handle_t me)
{
  assert(me);

  me->full = false;
  if (++(me->tail) == me->max)
  {
    me->tail = 0;
  }
}

int circular_buf_put(cbuf_handle_t me, AVFrame *data)
{
  int r = -1;

  if (!circ_buf_full(me))
  {
    me->bufferFrames[me->head] = data;
    advance_pointer(me);
    r = 0;
  }

  return r;
}


int circular_buf_put2(cbuf_handle_t me, AVFrame *data)
{
  me->bufferFrames[me->head] = data;
  advance_pointer(me);

  return 0;
}

int circular_buf_get(cbuf_handle_t me, AVFrame *data)
{
  int r = -1;

  if (!circ_buf_empty(me))
  {
    data = me->bufferFrames[me->tail];
    retreat_pointer(me);

    r = 0;
  }

  return r;
}

AVFrame* circular_buf_get2(cbuf_handle_t me)
{
  int r = -1;

  AVFrame *ret = allocPicture(AV_PIX_FMT_RGB24, WIN_WIDTH, WIN_HEIGHT);

  if (!circ_buf_empty(me))
  {
    ret = me->bufferFrames[me->tail];
    retreat_pointer(me);

    return ret;
  }

  return ret;
}

static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                   const char *filename)
{   
    int i=0;
    char buf[1024];
    int ret;
    AVFrame *pFrameRGB = NULL;
    pFrameRGB = allocPicture(AV_PIX_FMT_RGB24, width, height);
    AVFrame *avFrameDst = NULL;
    avFrameDst = allocPicture(AV_PIX_FMT_RGB24, width, height);

    if(pFrameRGB==NULL)
        exit(0);

    if(avFrameDst == NULL) {
        exit(0);
    }

    struct SwsContext *sws_ctx = NULL;
    
    sws_ctx = sws_getContext(dec_ctx->width,
        dec_ctx->height,
        pix_fmt,
        avFrameDst->width,
        avFrameDst->height,
        avFrameDst->format,
        SWS_FAST_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND,
        NULL,
        NULL,
        NULL
    );


    ret = avcodec_send_packet(dec_ctx, pkt);
 
    while (ret >= 0 && count<FRAMES ) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        sws_scale(sws_ctx, (uint8_t const * const *)frame->data,
		  frame->linesize, 0, frame->height,
		   avFrameDst->data, avFrameDst->linesize);
        sws_freeContext(sws_ctx);

        fflush(stdout);

        usleep(1000000 / (17 * 24 / FPS));
        printf("\nInsering frame into buffer\n");
        circular_buf_put(glob_buffer, avFrameDst);

         count++;
    }
}

void *producer_decode(void *arg)
{
  // acquired lock

  const char *filename, *outfilename;
  FILE *fptr;
  AVFrame *frame;
  uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
  uint8_t *data;
  size_t   data_size;
  int eof;
  AVPacket *pkt;
  int count = 0;
  filename    = "sample1.mpg";
  outfilename = "abc";

  //Initilizations
  const AVCodec *codec = NULL;
  AVCodecParserContext *parser;


  if(avformat_open_input(&pFormatCtx, filename, NULL, NULL) < 0){
      fprintf(stderr, "COuld not open source");
      exit(0); // Couldn't open file
  }

  if(avformat_find_stream_info(pFormatCtx, NULL) < 0) {
      fprintf(stderr, "COuld not find stream");
      exit(0);  
  }

  int stream_index, ret = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);

  AVStream *video_stream = NULL;


  if (ret < 0)
  {
      fprintf(stderr, "Could not find %s stream in input file '%s'\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO), filename);
      exit(0);
  }
  else
  {
      stream_index = ret;
      video_stream = pFormatCtx->streams[stream_index];
      codec = avcodec_find_decoder(video_stream->codecpar->codec_id); // find decoder for the stream
      if (!codec)
      {
          fprintf(stderr, "Failed to find %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
          exit(0);
      }
      pCodecCtx = avcodec_alloc_context3(codec); // Allocate a codec context for the decoder
      if (!pCodecCtx)
      {
          fprintf(stderr, "Failed to allocate the %s codec context\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
          exit(0);
      }
      if ((ret = avcodec_parameters_to_context(pCodecCtx, video_stream->codecpar)) < 0)
      {
          fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
          exit(0);
      }
      /* Init the decoders */
      if ((ret = avcodec_open2(pCodecCtx, codec, NULL)) < 0)
      {
          fprintf(stderr, "Failed to open %s codec\n", av_get_media_type_string(AVMEDIA_TYPE_VIDEO));
          exit(0);
      }
  }

  video_stream = pFormatCtx->streams[stream_index];

  width = pCodecCtx->width;
  height = pCodecCtx->height;
  pix_fmt = pCodecCtx->pix_fmt;

  ret = av_image_alloc(video_dst_data, video_dst_linesize, width, height, pix_fmt, 1);
  if (ret < 0)
  {
      fprintf(stderr, "Could not allocate raw video buffer\n");
      exit(ret);
  }
  video_dst_bufsize = ret;


  pkt = av_packet_alloc();
  if (!pkt)
      exit(1);

  /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
  memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

  parser = av_parser_init(codec->id);
  if (!parser) {
      fprintf(stderr, "parser not found\n");
      exit(1);
  }

  /* open it */
  if (avcodec_open2(pCodecCtx, codec, NULL) < 0) {
      fprintf(stderr, "Could not open codec\n");
      exit(1);
  }

  fptr = fopen(filename, "rb");
  if (!fptr) {
      fprintf(stderr, "Could not open %s\n", filename);
      exit(1);
  }

  srcFrame = av_frame_alloc();
  if (!srcFrame) {
      fprintf(stderr, "Could not allocate video frame\n");
      exit(1);
  }

  //Mutex starts
  pthread_mutex_lock(&lock);

  do {
    data_size = fread(inbuf, 1, INBUF_SIZE, fptr);
    if (ferror(fptr))
            break;
    eof = !data_size;
    data = inbuf;
    while (circ_buf_full(glob_buffer))
      {
        printf("\nPassing over to consumer\n");
        pthread_cond_wait(&c, &lock);
      }
     while (data_size > 0 || eof) {
      ret = av_parser_parse2(parser, pCodecCtx, &pkt->data, &pkt->size,
                              data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
      if (ret < 0) {
          fprintf(stderr, "Error while parsing\n");
          exit(1);
      }
      data      += ret;
      data_size -= ret;

      if (pkt->size)
          decode(pCodecCtx, srcFrame, pkt, outfilename);
      else if (eof)
          break;    
      }
      pthread_cond_signal(&d);
  }while(!eof);

  printf("Producer finished");
  pthread_mutex_unlock(&lock);

  decode(pCodecCtx, srcFrame, NULL, outfilename);
 
  fclose(fptr);
 
  av_parser_close(parser);
  avcodec_free_context(&pCodecCtx);
  av_frame_free(&srcFrame);
  av_packet_free(&pkt);
}


static gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, WIN_WIDTH, WIN_HEIGHT);
    unsigned char *current_row = cairo_image_surface_get_data(surface);
    int x, y, stride = cairo_image_surface_get_stride(surface);

    unsigned char *buf = consumer_buff->bufferFrames[frameIndex%BUFFERSIZE]->data[0];
    cairo_surface_flush(surface);
    int pixel_num = 1;
    for (y = 0; y < WIN_HEIGHT; ++y)
    {
        uint32_t *row = (void *)current_row;
        const int line_size = consumer_buff->bufferFrames[frameIndex%BUFFERSIZE]->linesize[0];
        for (x = 0; x < line_size / 3; ++x)
        {
            uint32_t r = *(buf + y * line_size + x * 3 + 0);
            uint32_t g = *(buf + y * line_size + x * 3 + 1);
            uint32_t b = *(buf + y * line_size + x * 3 + 2);
            row[x] = (r << 16) | (g << 8) | b;
        }
        current_row += stride;
    }
    frameIndex++;
    //usleep(1000000 / (17 * 24 / 50));
    cairo_surface_mark_dirty(surface);
    cairo_set_source_surface(cr, surface, 0, 0);
    cairo_paint(cr);

  return FALSE;
}

void *consumer_frames(gpointer user_data)
{
  consumerFrame = allocPicture(AV_PIX_FMT_RGB24, WIN_WIDTH, WIN_HEIGHT);
  char buf[1024];
  AVFrame *data;
  int j=0;
  // acquired lock
  pthread_mutex_lock(&lock);

  for (int i = 0; i < FRAMES; i++)
  {
    while (circ_buf_empty(glob_buffer))
    {
      printf("\nPassing back to producer\n");
      pthread_cond_wait(&d, &lock);
    }
    pthread_cond_signal(&c);

    usleep(1000000 / (17 * 24 / FPS));
    consumerFrame = circular_buf_get2(glob_buffer);
    circular_buf_put2(consumer_buff, consumerFrame);


    g_signal_connect(G_OBJECT(darea), "draw", G_CALLBACK(on_draw_event), user_data);
    gtk_widget_queue_draw(darea);
    
    printf("\nConsumning frame\n");
  }
  pthread_mutex_unlock(&lock);
  printf("\nConsumer finished\n");
}

static void activate(GtkApplication *app, unsigned char *user_data)
{
  int err2;
  pthread_t consume_thread;
  GtkWidget *window;
  window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

  darea = gtk_drawing_area_new();

  gtk_container_add(GTK_CONTAINER(window), darea);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  gtk_window_set_default_size(GTK_WINDOW(window), WIN_WIDTH, WIN_HEIGHT);
  gtk_window_set_title(GTK_WINDOW(window), "Video Player");

  gtk_widget_show_all(window);
  //sleep(2);

  err2 = pthread_create(&consume_thread, NULL, &consumer_frames, user_data);
  if (err2 != 0)
  {
    printf("\nCan't create thread2");
    exit(0);
  } else {
    printf("\nThread for consuming created!\n");
  }

  gtk_main();

}

int main(int argc, char **argv)
{

  glob_buffer = circular_buf_init(10);
  consumer_buff = circular_buf_init(10);
  buffer_init();

  pthread_cond_init(&c, NULL);
  pthread_cond_init(&d, NULL);

  int err1, err2;
  if (pthread_mutex_init(&lock, NULL) != 0)
  {
    printf("\n mutex init has failed\n");
    return 1;
  }

  /*create producer and consumer threads*/
  pthread_t produce_thread;

  err1 = pthread_create(&produce_thread, NULL, &producer_decode, NULL);
  if (err1 != 0)
  {
    printf("\nCan't create thread1");
    exit(0);
  } else {
    printf("\nProducer thread has started!\n");
  }

  GtkApplication *app = gtk_application_new("org.gtk.example", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  int status = g_application_run(G_APPLICATION(app), 1, argv);
  g_object_unref(app);
  return status;
}