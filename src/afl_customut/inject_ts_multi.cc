/*
    New Custom Mutator for AFL++
    Written by Khaled Yakdan <yakdan@code-intelligence.de>
                         Andrea Fioraldi <andreafioraldi@gmail.com>
                         Shengtuo Hu <h1994st@gmail.com>
                         Dominik Maier <mail@dmnk.co>
*/

// You need to use -I/path/to/AFLplusplus/include -I.
#include "afl-fuzz.h"
#include <sys/stat.h> 
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <mqueue.h>
#include <assert.h>
#include <iostream>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>

typedef struct patch_point{
    uint64_t addr;
    uint8_t reg_size;
} Patchpoint;
typedef struct test_case{
    char filename[255];
    char filehash[65];
    Patchpoint patch_point;
}TestCase;

typedef struct my_mutator {
    afl_state_t *afl;
    unsigned char *buf;
    unsigned char *msg_buf;
    uint64_t file_count;
    Patchpoint last_mut;
    bool success;
    struct mq_attr my_mqattr;
    mqd_t mqd;
    unsigned int mq_pri;

} my_mutator_t;
#define MQNAME "/FTMM_MQ"

extern "C" {

my_mutator_t *afl_custom_init(afl_state_t *afl, unsigned int seed) {

    my_mutator_t *data = (my_mutator_t *)calloc(1, sizeof(my_mutator_t));
    if (!data) {
        perror("afl_custom_init calloc");
        return NULL;
    }

    if ((data->buf = (unsigned char *)malloc(MAX_FILE)) == NULL) {
        perror("afl_custom_init malloc");
        return NULL;
    }

    data->my_mqattr.mq_flags = 0;
    data->my_mqattr.mq_maxmsg = 10;
    data->my_mqattr.mq_msgsize = sizeof(TestCase);
    data->my_mqattr.mq_curmsgs = 0;
    mqd_t mqd = mq_open (MQNAME, O_CREAT | O_RDWR,  0600, &data->my_mqattr);
    if (mqd == -1){
        perror("mq_open");
        return NULL;
    }
    // struct mq_attr old_attr;
    // data->my_mqattr.mq_flags = O_NONBLOCK;
    // assert(mq_getattr(mqd, &old_attr) != -1);
    // assert(mq_setattr(mqd, &data->my_mqattr, &old_attr) != -1);

    data->mqd = mqd;
    data->file_count = 0;
    data->afl = afl;
    if ((data->msg_buf = (unsigned char *)malloc(sizeof(TestCase))) == NULL) {
        perror("afl_custom_init malloc");
        return NULL;
    }
    data->mq_pri = 1;


    return data;

}


size_t afl_custom_post_process(my_mutator_t *data, unsigned char *in_buf, size_t buf_size, unsigned char **out_buf) {
    //return buf_size;
    // default discard
    size_t out_size = 0;
    *out_buf = nullptr;

    int ret = mq_receive(data->mqd, (char *)data->msg_buf, data->my_mqattr.mq_msgsize, &data->mq_pri);
    if (ret == -1) {
        if (errno == EAGAIN){
            //printf("MQ currently empty.\n");
        }else{
            perror("mq_receive");
        }
        return out_size;
    }
    
    TestCase *ts_ptr = (TestCase *)data->msg_buf;
    if (strcmp(ts_ptr->filename, "") != 0){
        data->file_count++;
        data->success = true;
        *out_buf = data->buf;
        // read content
        std::string filename = ts_ptr->filename;
        std::stringstream buf;
        std::ifstream mutated_output(filename);
        if (mutated_output.is_open()){
            mutated_output.read(reinterpret_cast<char *>(*out_buf), MAX_FILE);
            out_size = mutated_output.gcount();
        }

        // buf << mutated_output.rdbuf();
        // // set out_buf and buf_size
        // memcpy(*out_buf, (buf.str()).c_str(), MAX_FILE_SIZE);
        // buf_size = strlen((char *)(*out_buf));
        // (*out_buf)[MAX_FILE_SIZE - 1] = '\0';

        data->last_mut.addr = ts_ptr->patch_point.addr;
        data->last_mut.reg_size = ts_ptr->patch_point.reg_size;
        //printf("Addr: %ld, Value: %ld\n", ts_ptr->patch_point.addr, ts_ptr->patch_point.injectValue);
        //printf("Content: \n%s\n", (buf.str()).c_str());
        std::filesystem::remove(filename);
    }else{
        data->success = false;
    }

    return out_size;

}

void afl_custom_deinit(my_mutator_t *data) {

    free(data->buf);
    free(data);
    free(data->msg_buf);
    mq_close(data->mqd);

}

}


