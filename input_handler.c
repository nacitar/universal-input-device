/*
 *  Copyright (c) 1999-2001 Vojtech Pavlik
 */

/*
 *  Input driver event debug module - dumps all events into syslog
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so either by
 * e-mail - mail your message to <vojtech@ucw.cz>, or by paper mail:
 * Vojtech Pavlik, Simunkova 1594, Prague 8, 182 00 Czech Republic
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "info_strings.h"
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/device.h>

#include "controller_id.h"

MODULE_AUTHOR("Jacob McIntosh <nacitar@ubercpp.com>");
MODULE_DESCRIPTION("Module to hook into the kernel's input subsystem.");
MODULE_LICENSE("GPL");


#define HANDLER_NAME "universal_input_device"

// Track all deviced by name, id, and physical location (not dev node)


// https://www.kernel.org/doc/Documentation/input/event-codes.txt
// https://github.com/torvalds/linux/blob/master/include/uapi/linux/input-event-codes.h

// figure out how to handle bluetooth!
// upon connecting, identify the controller, see if it has settings stored.
// use the pointer to lookup the value in a sorted list(set)
// figure out a way to enter programming mode, and setup bindings.
// figure out a way to save those bindings
// ignore axis movement that isn't 50% or more (how do we know extents?)
// need some sort of external button for programming inputs and pairing.


// input_value is type code and value
// input_dev.absinfo


// break axes into positive and negative directions
#define UGC_MAX_DEVICES 256u
#define UGC_CONFIGURE_REPEAT_COUNT 10u
#define UGC_MAX_INPUTS 50
#define UGC_ID_TO_INDEX(name) (+(unsigned char)*(name))

struct ugc_input {
  unsigned int type;
  unsigned int code;
};

enum ugc_config_state {
  CONNECTED=0, CONFIGURING, READY
};

// start with final button to configure; store that as terminal
struct ugc_device {
  struct input_dev *dev;  // name, uniq, phys, id.bustype
  enum ugc_config_state config_state;
  unsigned int count;  // repeat count in CONFIGURING state, button count in READY state
  struct ugc_input last_input;
  struct uint_map input_nodes[UGC_MAX_INPUTS];  // storage for nodes of the tree
  struct rb_root input_code_to_index;  // = RB_ROOT; but that just zeroes...
  __u32 input_state[UGC_MAX_INPUTS];
};

struct ugc_device_group {
  unsigned long acquiredbit[BITS_TO_LONGS(UGC_MAX_DEVICES)];
  unsigned int num_acquired;
};

// 2 chars, [0] = a char id, [1] = null terminator
// Index 0 is "\0", empty string... but valid still.
const char ugc_device_id[UGC_MAX_DEVICES][2] = {
#define UGC_ID_GEN(offset) \
  {(char)offset+0}, {(char)offset+1}, {(char)offset+2}, {(char)offset+3}, \
  {(char)offset+4}, {(char)offset+5}, {(char)offset+6}, {(char)offset+7}, \
  {(char)offset+8}, {(char)offset+9}, {(char)offset+10}, {(char)offset+11}, \
  {(char)offset+12}, {(char)offset+13}, {(char)offset+14}, {(char)offset+15}
  UGC_ID_GEN(0x00), UGC_ID_GEN(0x10), UGC_ID_GEN(0x20), UGC_ID_GEN(0x30),
  UGC_ID_GEN(0x40), UGC_ID_GEN(0x50), UGC_ID_GEN(0x60), UGC_ID_GEN(0x70),
  UGC_ID_GEN(0x80), UGC_ID_GEN(0x90), UGC_ID_GEN(0xA0), UGC_ID_GEN(0xB0),
  UGC_ID_GEN(0xC0), UGC_ID_GEN(0xD0), UGC_ID_GEN(0xE0), UGC_ID_GEN(0xF0)
#undef UGC_ID_GEN
};

const char* ugc_device_id_acquire(struct ugc_device_group* group) {
  if (group->num_acquired < UGC_MAX_DEVICES) {
    const int index = find_first_zero_bit(group->acquiredbit, UGC_MAX_DEVICES);
    set_bit(index, group->acquiredbit);
    ++group->num_acquired;
    return ugc_device_id[index];
  }
  printk(KERN_DEBUG pr_fmt("Cannot acquire id; max devices reached.\n"));
  return NULL;
}

void ugc_device_id_release(struct ugc_device_group* group,
    const char* id) {
  if (test_and_clear_bit(UGC_ID_TO_INDEX(id), group->acquiredbit)) {
    --group->num_acquired;
  } else {
    printk(KERN_DEBUG pr_fmt("Cannot release id %d; it is not acquired.\n"),
        UGC_ID_TO_INDEX(id));
  }
}


struct ugc_device_group g_device_group = {0};
struct ugc_device g_devices[UGC_MAX_DEVICES];


static int ugc_connect_device(struct input_handler *handler, struct input_dev *dev,
    const struct input_device_id *id)
{
  struct input_handle *handle;
  int error;
  const char* bus_name;
  const char* device_id;
  handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
  if (!handle) {
    return -ENOMEM;
  }

  device_id = ugc_device_id_acquire(&g_device_group);
  if (!device_id) {
    printk(KERN_DEBUG pr_fmt("Device connected, but no IDs available.\n"));
    return 0;
  }
  handle->dev = dev;
  handle->handler = handler;
  handle->name = device_id;

  error = input_register_handle(handle);
  if (error)
    goto err_free_handle;

  error = input_open_device(handle);
  if (error)
    goto err_unregister_handle;

  g_devices[UGC_ID_TO_INDEX(device_id)] = (struct ugc_device) {
    .dev = dev,
    .input_code_to_index = RB_ROOT
  };

  get_bus_name(dev->id.bustype, &bus_name);

  printk(KERN_DEBUG pr_fmt("Connected device: [%s] %s (%s) at %s\n"),
      bus_name,
      dev->name ?: "unknown",
      dev->uniq ?: "unknown",
      dev->phys ?: "unknown");

  return 0;

err_unregister_handle:
  input_unregister_handle(handle);
err_free_handle:
  kfree(handle);
  return error;
}

static void ugc_disconnect_device(struct input_handle *handle)
{
  const char* bus_name;
  get_bus_name(handle->dev->id.bustype, &bus_name);

  // no need to cleanup the device itself; all storage is static and
  // it is cleared when reused
  ugc_device_id_release(&g_device_group, handle->name);

  printk(KERN_DEBUG pr_fmt("Disconnected device: [%s] %s (%s) at %s\n"),
      bus_name,
      handle->dev->name ?: "unknown",
      handle->dev->uniq ?: "unknown",
      handle->dev->phys ?: "unknown");

  input_close_device(handle);
  input_unregister_handle(handle);
  kfree(handle);
}

static void ugc_event(struct input_handle *handle, unsigned int type, unsigned int code, int value)
{
  const char *event_name, *code_name, *bus_name;
  struct ugc_device *device;
  get_event_name(type, code, &event_name, &code_name);
  get_bus_name(handle->dev->id.bustype, &bus_name);
  if (!event_name) {
    event_name = "UNKNOWN";
  }
  if (!code_name) {
    code_name = "UNKNOWN";
  }
  printk(KERN_DEBUG pr_fmt("Event. Dev: %s, Type: %s[%d], Code: %s[%d], Value: %d\n"),
      dev_name(&handle->dev->dev), event_name, type, code_name, code, value);

  device = g_devices + UGC_ID_TO_INDEX(handle->name);


  if (type == EV_KEY) {
    if (value != 0) {  // pressed
      switch (device->config_state) {
        case CONNECTED: {
          if (type == device->last_input.type &&
              code == device->last_input.code) {
            if (++device->count == 10) {
              device->config_state = CONFIGURING;
              device->count = 0;
            }
          } else {
            device->last_input.type = type;
            device->last_input.code = code;
            device->count = 1;
          }
          break;
        }
        case CONFIGURING: {
          struct uint_map *node;
          if (code == device->last_input.code && device->count == 0) {
            // first input can't be terminal
            break;
          }
          node = &device->input_nodes[device->count];
          node->key = code;
          node->value = device->count;
          printk(KERN_DEBUG pr_fmt("Adding button: %u, Code %u\n"),
              node->value, node->key);
          if (!uint_map_insert(&device->input_code_to_index, node)) {
            printk(KERN_DEBUG pr_fmt("FAIL\n"));
          }
          ++device->count;
          // TODO: need to make the key type and code based
          // TODO: need to forbid repeat bindings
          if (code == device->last_input.code) {
            device->config_state = READY;
          }
          break;
        }
        case READY: {
          struct uint_map *node;
          node = uint_map_search(&device->input_code_to_index, code);
          if (node) {
            int i;
            device->input_state[node->value] = value;
            printk(KERN_DEBUG pr_fmt("Button: %u, Value: %u\n"),
                node->value, value);
          }
          break;
        }
      }
    }
  }
}
static const struct input_device_id ugc_id_match_table[] = {
  { .driver_info = 1 },	/* Matches all devices */
  { },			/* Terminating zero entry */
};

MODULE_DEVICE_TABLE(input, ugc_id_match_table);

static struct input_handler ugc_handler = {
  .event =	ugc_event,
  .connect =	ugc_connect_device,
  .disconnect =	ugc_disconnect_device,
  .name =		HANDLER_NAME,
  .id_table =	ugc_id_match_table,
};

static int __init ugc_init(void)
{
  return input_register_handler(&ugc_handler);
}

static void __exit ugc_exit(void)
{
  input_unregister_handler(&ugc_handler);
}

module_init(ugc_init);
module_exit(ugc_exit);
