// License: Apache 2.0. See LICENSE file in root directory.
// Copyright(c) 2015 Intel Corporation. All Rights Reserved.

#include <librealsense/rs.hpp>
#include "example.hpp"

#include <sstream>
#include <iostream>
#include <iomanip>
#include <thread>
#include <algorithm>

texture_buffer buffers[RS_STREAM_COUNT];
bool align_depth_to_color = false;
bool align_color_to_depth = false;
bool color_rectification_enabled = false;

#include <memory>

#include <arpa/inet.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SLEEP_US 33333 // 30FPS
#define MAGIC 0x17923349ab10ea9aL

static int64_t utime_now()
{
    struct timeval tv;
    gettimeofday (&tv, NULL);
    return (int64_t) tv.tv_sec * 1000000 + tv.tv_usec;
}

static void write_i32(uint8_t *buf, int32_t v)
{
    buf[0] = (v >> 24) & 0xFF;
    buf[1] = (v >> 16) & 0xFF;
    buf[2] = (v >>  8) & 0xFF;
    buf[3] = (v      ) & 0xFF;
}

static void write_i64(uint8_t *buf, int64_t v)
{
    uint32_t h = (uint32_t) (v >> 32);
    uint32_t l = (uint32_t) (v);

    write_i32(buf+0, h);
    write_i32(buf+4, l);
}

int main(int argc, char * argv[]) try
{
    // tcp stuff
    setlinebuf(stdout);
    setlinebuf(stderr);

    if (argc != 3) {
        printf("Usage: tcptest <host> <port>\n");
        exit(1);
    }

    char *host = argv[1];
    int   port = atoi(argv[2]);

    printf("Connecting to '%s' on port %d\n", host, port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock  < 0) {
        printf("Error opening socket\n");
        perror("socket: ");
        if (sock >= 0)            close(sock);
        return 0;
    }

    struct hostent *server = gethostbyname(host);
    if (server == NULL) {
        printf("Error getting host by name\n");
        perror("gethostbyname: ");
        if (sock >= 0)            close(sock);
        return 0;
    }

    struct sockaddr_in serv_addr;
    bzero((char*) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(port);
    bcopy((char *)server->h_addr,
          (char *)&serv_addr.sin_addr.s_addr,
          server->h_length);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Error connecting to socket\n");
        perror("connect: ");
        if (sock >= 0)            close(sock);
        return 0;
    }

    printf("Connected\n");
    ///////////////////////
  
    rs::log_to_console(rs::log_severity::warn);
    //rs::log_to_file(rs::log_severity::debug, "librealsense.log");

    rs::context ctx;
    if(ctx.get_device_count() == 0) throw std::runtime_error("No device detected. Is it plugged in?");
    rs::device & dev = *ctx.get_device(0);

    dev.enable_stream(rs::stream::depth, rs::preset::best_quality);
    dev.enable_stream(rs::stream::color, rs::preset::best_quality);
    dev.enable_stream(rs::stream::infrared, rs::preset::best_quality);
    try { dev.enable_stream(rs::stream::infrared2, 0, 0, rs::format::any, 0); } catch(...) {}

    // Compute field of view for each enabled stream
    for(int i = 0; i < 4; ++i)
    {
        auto stream = rs::stream(i);
        if(!dev.is_stream_enabled(stream)) continue;
        auto intrin = dev.get_stream_intrinsics(stream);
        std::cout << "Capturing " << stream << " at " << intrin.width << " x " << intrin.height;
        std::cout << std::setprecision(1) << std::fixed << ", fov = " << intrin.hfov() << " x " << intrin.vfov() << ", distortion = " << intrin.model() << std::endl;
    }
    
    // Start our device
    dev.start();

    // Open a GLFW window
    glfwInit();
    std::ostringstream ss; ss << "CPP Capture Example (" << dev.get_name() << ")";
    GLFWwindow * win = glfwCreateWindow(1280, 960, ss.str().c_str(), 0, 0);
    glfwSetWindowUserPointer(win, &dev);
    glfwSetKeyCallback(win, [](GLFWwindow * win, int key, int scancode, int action, int mods) 
    { 
        auto dev = reinterpret_cast<rs::device *>(glfwGetWindowUserPointer(win));
        if(action != GLFW_RELEASE) switch(key)
        {
        case GLFW_KEY_R: color_rectification_enabled = !color_rectification_enabled; break;
        case GLFW_KEY_C: align_color_to_depth = !align_color_to_depth; break;
        case GLFW_KEY_D: align_depth_to_color = !align_depth_to_color; break;
        case GLFW_KEY_E:
            if(dev->supports_option(rs::option::r200_emitter_enabled))
            {
                int value = !dev->get_option(rs::option::r200_emitter_enabled);
                std::cout << "Setting emitter to " << value << std::endl;
                dev->set_option(rs::option::r200_emitter_enabled, value);
            }
            break;
        case GLFW_KEY_A:
            if(dev->supports_option(rs::option::r200_lr_auto_exposure_enabled))
            {
                int value = !dev->get_option(rs::option::r200_lr_auto_exposure_enabled);
                std::cout << "Setting auto exposure to " << value << std::endl;
                dev->set_option(rs::option::r200_lr_auto_exposure_enabled, value);
            }
            break;
        }
    });
    glfwMakeContextCurrent(win);

 ///////////////////////////////// TCP
    int64_t magic = MAGIC;

    int64_t utime = utime_now();

    int32_t width  = 640;
    int32_t height =  480;

    char format[] = "RGB";
    int32_t formatlen = strlen(format);

    int32_t imlen = width*height*3*sizeof(uint8_t);


    int32_t buflen = 8 + 8 + 4 + 4 + 4 + formatlen + 4 + imlen;

    uint8_t *buf = (uint8_t*)calloc(buflen, sizeof(uint8_t));
    while (!glfwWindowShouldClose(win))
    {
        // Wait for new images
        glfwPollEvents();
        dev.wait_for_frames();

        // Retrieve color data, which was previously configured as a 640 x 480 x 3 image of 8-bit color values
        const uint8_t * color_frame = reinterpret_cast<const uint8_t *>(dev.get_frame_data(rs::stream::color));


        const uint8_t *im = color_frame;
        uint8_t *ptr = buf;

        write_i64(ptr, magic);          ptr += 8;
        write_i64(ptr, utime);          ptr += 8;
        write_i32(ptr, width);          ptr += 4;
        write_i32(ptr, height);         ptr += 4;
        write_i32(ptr, formatlen);      ptr += 4;
        memcpy(ptr, format, formatlen); ptr += formatlen;
        write_i32(ptr, imlen);          ptr += 4;
        memcpy(ptr, im, imlen);         ptr += imlen;


        int len = buflen;
        
        int bytes = send(sock, buf, len, 0);


        if (bytes != len) {
            printf("Tried to send %d bytes, sent %d\n", len, bytes);
            perror("send: ");
            break;
        }
        usleep(SLEEP_US);

        // Clear the framebuffer
        int w,h;
        glfwGetFramebufferSize(win, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT);

        // Draw the images        
        glPushMatrix();
        glfwGetWindowSize(win, &w, &h);
        glOrtho(0, w, h, 0, -1, +1);
        buffers[0].show(dev, align_color_to_depth ? rs::stream::color_aligned_to_depth : (color_rectification_enabled ? rs::stream::rectified_color : rs::stream::color), 0, 0, w/2, h/2);
        buffers[1].show(dev, align_depth_to_color ? (color_rectification_enabled ? rs::stream::depth_aligned_to_rectified_color : rs::stream::depth_aligned_to_color) : rs::stream::depth, w/2, 0, w-w/2, h/2);
        buffers[2].show(dev, rs::stream::infrared, 0, h/2, w/2, h-h/2);
        buffers[3].show(dev, rs::stream::infrared2, w/2, h/2, w-w/2, h-h/2);
        

        glPopMatrix();
        glfwSwapBuffers(win);
    }

    free(buf);

    if (sock >= 0)
        close(sock);

    printf("Done\n");

    glfwDestroyWindow(win);
    glfwTerminate();
    return EXIT_SUCCESS;
}
catch(const rs::error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}
catch(const std::exception & e)
{
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
}
