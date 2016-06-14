// Copyright (c) 2016, Intel Corporation.

// Zephyr includes
#include <misc/util.h>

// ZJS includes
#include "zjs_aio.h"
#include "zjs_ipm.h"
#include "zjs_util.h"

DEFINE_SEMAPHORE(SEM_AIO_BLOCK);

/*
 * The analog input pin and channel number mapping
 * for Arduino 101 board.
 * A0 Channel 10
 * A1 Channel 11
 * A2 Channel 12
 * A3 Channel 13
 * A4 Channel 14
 */
#define A0 10
#define A1 11
#define A2 12
#define A3 13
#define A4 14

static uint32_t pin_values[5] = {};

struct zjs_cb_list_item {
    jerry_object_t *pin_obj;
    struct zjs_callback zjs_cb;
    double value;
    struct zjs_cb_list_item *next;
};

static struct zjs_cb_list_item *zjs_cb_list = NULL;

static struct zjs_cb_list_item *zjs_aio_callback_alloc()
{
    // effects: allocates a new callback list item and adds it to the list
    struct zjs_cb_list_item *item;
    item = task_malloc(sizeof(struct zjs_cb_list_item));
    if (!item) {
        PRINT("error: out of memory allocating callback struct\n");
        return NULL;
    }

    item->next = zjs_cb_list;
    zjs_cb_list = item;
    return item;
}

static void zjs_aio_callback_free(uintptr_t handle)
{
    // requires: handle is the native pointer we registered with
    //             jerry_set_object_native_handle
    //  effects: frees the callback list item for the given pin object
    struct zjs_cb_list_item **pItem = &zjs_cb_list;
    while (*pItem) {
        if ((uintptr_t)*pItem == handle) {
            *pItem = (*pItem)->next;
            task_free((void *)handle);
        }
        pItem = &(*pItem)->next;
    }
}

struct zjs_cb_list_item *zjs_aio_get_callback_item(uint32_t pin)
{

    // calls the JS callback registered in the struct
    struct zjs_cb_list_item **pItem = &zjs_cb_list;
    while (*pItem) {
        jerry_object_t *obj = (*pItem)->pin_obj;
        uint32_t pin_val;
        zjs_obj_get_uint32(obj, "pin", &pin_val);

        if (pin == pin_val) {
            return (*pItem);
        }
        pItem = &(*pItem)->next;
    }

    return NULL;
}

static void zjs_aio_call_function(struct zjs_callback *cb)
{
    // requires: called only from task context
    //  effects: handles execution of the JS callback when ready
    struct zjs_cb_list_item *mycb = CONTAINER_OF(cb, struct zjs_cb_list_item,
                                                 zjs_cb);
    jerry_value_t rval, arg;
    arg.type = JERRY_DATA_TYPE_FLOAT64;
    arg.u.v_float64 = mycb->value;
    if (jerry_call_function(mycb->zjs_cb.js_callback, NULL, &rval, &arg, 1))
        jerry_release_value(&rval);
}

int zjs_aio_ipm_send(uint32_t type, uint32_t pin, uint32_t value) {
    struct zjs_ipm_message msg;
    msg.block = false;
    msg.type = type;
    msg.pin = pin;
    msg.value = value;
    return zjs_ipm_send(MSG_ID_AIO, &msg, sizeof(msg));
}

int zjs_aio_ipm_send_blocking(uint32_t type, uint32_t pin, uint32_t value) {
    struct zjs_ipm_message msg;
    msg.block = true;
    msg.type = type;
    msg.pin = pin;
    msg.value = value;
    return zjs_ipm_send(MSG_ID_AIO, &msg, sizeof(msg));
}

// callback that gets updated of latest analog value from pin
void ipm_msg_receive_callback(void *context, uint32_t id, volatile void *data)
{
    if (id != MSG_ID_AIO)
        return;

    struct zjs_ipm_message *msg = (struct zjs_ipm_message *) data;

    if (msg->type == TYPE_AIO_OPEN_SUCCESS) {
        PRINT("pin %lu is opened\n", msg->pin);
    } else if (msg->type == TYPE_AIO_PIN_READ_SUCCESS) {
        if (msg->pin < A0 || msg->pin > A4) {
            PRINT("X86 - pin #%lu out of range\n", msg->pin);
            return;
        }

        if (msg->block) {
            pin_values[msg->pin-A0] = msg->value;
        }
        else {
            struct zjs_cb_list_item *mycb = zjs_aio_get_callback_item(msg->pin);

            if (mycb->zjs_cb.js_callback) {
                // TODO: ensure that there is no outstanding callback of this
                //   type or else we may be overwriting the value it should have
                //   reported
                mycb->value = (double)msg->value;
                zjs_queue_callback(&mycb->zjs_cb);
            }
        }
    } else {
        PRINT("IPM message not handled %lu\n", msg->type);
    }

    if (msg->block) {
        // un-block sync api
        isr_sem_give(SEM_AIO_BLOCK);
    }
}

jerry_object_t *zjs_aio_init()
{
    zjs_ipm_init();
    zjs_ipm_register_callback(MSG_ID_AIO, ipm_msg_receive_callback);

    // create global AIO object
    jerry_object_t *aio_obj = jerry_create_object();
    zjs_obj_add_function(aio_obj, zjs_aio_open, "open");
    return aio_obj;
}

bool zjs_aio_open(const jerry_object_t *function_obj_p,
                  const jerry_value_t *this_p,
                  jerry_value_t *ret_val_p,
                  const jerry_value_t args_p[],
                  const jerry_length_t args_cnt)
{
    if (args_cnt < 1 || !ZJS_IS_OBJ(args_p[0])) {
        PRINT("zjs_aio_open: invalid arguments\n");
        return false;
    }

    jerry_object_t *data = args_p[0].u.v_object;

    uint32_t device;
    if (!zjs_obj_get_uint32(data, "device", &device)) {
        PRINT("zjs_aio_open: missing required field (device)\n");
        return false;
    }

    uint32_t pin;
    if (!zjs_obj_get_uint32(data, "pin", &pin)) {
        PRINT("zjs_aio_open: missing required field (pin)\n");
        return false;
    }

    const int BUFLEN = 32;
    char buffer[BUFLEN];

    if (zjs_obj_get_string(data, "name", buffer, BUFLEN)) {
        buffer[0] = '\0';
    }

    char *name = buffer;
    bool raw = false;
    zjs_obj_get_boolean(data, "raw", &raw);

    // send IPM message to the ARC side
    zjs_aio_ipm_send_blocking(TYPE_AIO_OPEN, pin, 0);
    // block until reply or timeout
    if (task_sem_take(SEM_AIO_BLOCK, 500) == RC_TIME) {
        PRINT("Reply from ARC timed out!");
        return false;
    }

    // create the AIOPin object
    jerry_object_t *pinobj = jerry_create_object();
    zjs_obj_add_function(pinobj, zjs_aio_pin_read, "read");
    zjs_obj_add_function(pinobj, zjs_aio_pin_read_async, "read_async");
    zjs_obj_add_function(pinobj, zjs_aio_pin_abort, "abort");
    zjs_obj_add_function(pinobj, zjs_aio_pin_close, "close");
    zjs_obj_add_string(pinobj, name, "name");
    zjs_obj_add_uint32(pinobj, device, "device");
    zjs_obj_add_uint32(pinobj, pin, "pin");
    zjs_obj_add_boolean(pinobj, raw, "raw");

    *ret_val_p = jerry_create_object_value(pinobj);
    return true;
}

bool zjs_aio_pin_read(const jerry_object_t *function_obj_p,
                      const jerry_value_t *this_p,
                      jerry_value_t *ret_val_p,
                      const jerry_value_t args_p[],
                      const jerry_length_t args_cnt)
{
    jerry_object_t *obj = jerry_get_object_value(this_p);

    uint32_t device, pin;
    zjs_obj_get_uint32(obj, "device", &device);
    zjs_obj_get_uint32(obj, "pin", &pin);

    if (pin < A0 || pin > A4) {
        PRINT("pin #%lu out of range\n", pin);
        return false;
    }

    // send IPM message to the ARC side
    zjs_aio_ipm_send_blocking(TYPE_AIO_PIN_READ, pin, 0);
    // block until reply or timeout
    if (task_sem_take(SEM_AIO_BLOCK, 500) == RC_TIME) {
        PRINT("Reply from ARC timed out!");
        return false;
    }

    double value;
    value = (double) pin_values[pin-A0];

    ret_val_p->type = JERRY_DATA_TYPE_FLOAT64;
    ret_val_p->u.v_float64 = value;
    return true;
}

bool zjs_aio_pin_abort(const jerry_object_t *function_obj_p,
                       const jerry_value_t *this_p,
                       jerry_value_t *ret_val_p,
                       const jerry_value_t args_p[],
                       const jerry_length_t args_cnt)
{
    // NO-OP
    return true;
}

bool zjs_aio_pin_close(const jerry_object_t *function_obj_p,
                       const jerry_value_t *this_p,
                       jerry_value_t *ret_val_p,
                       const jerry_value_t args_p[],
                       const jerry_length_t args_cnt)
{
    // NO-OP
    return true;
}

// Asynchrounous Operations
bool zjs_aio_pin_read_async(const jerry_object_t *function_obj_p,
                            const jerry_value_t *this_p,
                            jerry_value_t *ret_val_p,
                            const jerry_value_t args_p[],
                            const jerry_length_t args_cnt)
{
    if (args_cnt < 1 || !ZJS_IS_OBJ(args_p[0])) {
        PRINT("zjs_aio_pin_read_async: invalid argument\n");
        return false;
    }

    jerry_object_t *obj = jerry_get_object_value(this_p);
    uint32_t device, pin;
    zjs_obj_get_uint32(obj, "device", &device);
    zjs_obj_get_uint32(obj, "pin", &pin);

    struct zjs_cb_list_item *item = zjs_aio_get_callback_item(pin);
    if (!item) {
        item = zjs_aio_callback_alloc();
    }

    if (!item)
        return false;

    item->pin_obj = obj;
    item->zjs_cb.js_callback = args_p[0].u.v_object;
    item->zjs_cb.call_function = zjs_aio_call_function;

    jerry_acquire_object(item->zjs_cb.js_callback);

    // watch for the object getting garbage collected, and clean up
    jerry_set_object_native_handle(obj, (uintptr_t)item,
                                       zjs_aio_callback_free);

    // send IPM message to the ARC side and wait for reponse
    zjs_aio_ipm_send(TYPE_AIO_PIN_READ, pin, 0);
    return true;
}
