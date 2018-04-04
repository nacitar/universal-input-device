#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "info_strings.h"
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/device.h>

#include "controller_id.h"
#include "ugc_input.h"

#include <linux/interrupt.h>
#include <linux/gpio.h>

MODULE_AUTHOR("Jacob McIntosh <nacitar@ubercpp.com>");
MODULE_DESCRIPTION("Allows usage of arbitrary input devices on "
    "retro game consoles.");
MODULE_LICENSE("GPL");


#define HANDLER_NAME "universal_game_controller"



#define GPIO_INTERRUPT_LABEL "ugc_test_interrupt"
#define GPIO_DEVICE_LABEL "ugc_device"
#define GPIO_INTERRUPT_PIN 17

short int irq_any_gpio = 0;
static irqreturn_t r_irq_handler(int irq, void *dev_id, struct pt_regs *regs) {
 
   unsigned long flags;
   
   // disable hard interrupts (remember them in flag 'flags')
   local_irq_save(flags);
 
   // NOTE:
   // Anonymous Sep 17, 2013, 3:16:00 PM:
   // You are putting printk while interupt are disabled. printk can block.
   // It's not a good practice.
   // 
   // hardware.coder:
   // http://stackoverflow.com/questions/8738951/printk-inside-an-interrupt-handler-is-it-really-that-bad
 
   printk(KERN_NOTICE "Interrupt [%d] for device %s was triggered !.\n",
          irq, (char *) dev_id);
 
   // restore hard interrupts
   local_irq_restore(flags);
 
   return IRQ_HANDLED;
}

void r_int_config(void) {

   if (gpio_request(GPIO_INTERRUPT_PIN, GPIO_INTERRUPT_LABEL)) {
      printk("GPIO request faiure: %s\n", GPIO_INTERRUPT_LABEL);
      return;
   }

   if ( (irq_any_gpio = gpio_to_irq(GPIO_INTERRUPT_PIN)) < 0 ) {
      printk("GPIO to IRQ mapping faiure %s\n", GPIO_INTERRUPT_LABEL);
      return;
   }

   printk(KERN_NOTICE "Mapped int %d\n", irq_any_gpio);

   if (request_irq(irq_any_gpio,
                   (irq_handler_t ) r_irq_handler,
                   IRQF_TRIGGER_FALLING,
                   GPIO_INTERRUPT_LABEL,
                   GPIO_DEVICE_LABEL)) {
      printk("Irq Request failure\n");
      return;
   }

   return;
}

void r_int_release(void) {

   free_irq(irq_any_gpio, GPIO_DEVICE_LABEL);
   gpio_free(GPIO_INTERRUPT_PIN);

   return;
}

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
#define UGC_NAME_TO_INDEX(name) (+(unsigned char)*(name))

// axis < 0 and > 0 distinct inputs
const __u32 g_UGC_MAX_VALUE = U32_MAX;
const __u32 g_UGC_MIN_PRESSED_VALUE = U32_MAX / 2;

enum ugc_config_state {
  CONNECTED=0, CONFIGURING, READY
};

// start with final button to configure; store that as terminal
struct ugc_device {
  struct input_dev *dev;  // name, uniq, phys, id.bustype
  enum ugc_config_state config_state;
  unsigned int count;  // repeat count in CONFIGURING state, button count in READY state
  struct ugc_input last_input;
  struct ugc_input input_nodes[UGC_MAX_INPUTS];  // storage for nodes of the tree
  struct rb_root input_code_to_index;  // = RB_ROOT; but that just zeroes...
  __u32 input_state[UGC_MAX_INPUTS];
};

struct ugc_device_group {
  unsigned long acquiredbit[BITS_TO_LONGS(UGC_MAX_DEVICES)];
  unsigned int num_acquired;
};

// 2 chars, [0] = a char id, [1] = null terminator
// Index 0 is "\0", empty string... but valid still.
const char ugc_device_name[UGC_MAX_DEVICES][2] = {
#define UGC_NAME_PT(offset) \
  {(char)offset+0}, {(char)offset+1}, {(char)offset+2}, {(char)offset+3}, \
  {(char)offset+4}, {(char)offset+5}, {(char)offset+6}, {(char)offset+7}, \
  {(char)offset+8}, {(char)offset+9}, {(char)offset+10}, {(char)offset+11}, \
  {(char)offset+12}, {(char)offset+13}, {(char)offset+14}, {(char)offset+15}
  UGC_NAME_PT(0x00), UGC_NAME_PT(0x10), UGC_NAME_PT(0x20), UGC_NAME_PT(0x30),
  UGC_NAME_PT(0x40), UGC_NAME_PT(0x50), UGC_NAME_PT(0x60), UGC_NAME_PT(0x70),
  UGC_NAME_PT(0x80), UGC_NAME_PT(0x90), UGC_NAME_PT(0xA0), UGC_NAME_PT(0xB0),
  UGC_NAME_PT(0xC0), UGC_NAME_PT(0xD0), UGC_NAME_PT(0xE0), UGC_NAME_PT(0xF0)
#undef UGC_NAME_PT
};

const char* ugc_device_name_acquire(struct ugc_device_group* group) {
  if (group->num_acquired < UGC_MAX_DEVICES) {
    const int index = find_first_zero_bit(group->acquiredbit, UGC_MAX_DEVICES);
    set_bit(index, group->acquiredbit);
    ++group->num_acquired;
    return ugc_device_name[index];
  }
  printk(KERN_DEBUG pr_fmt("Cannot acquire name; max devices reached.\n"));
  return NULL;
}

void ugc_device_name_release(struct ugc_device_group* group,
    const char* name) {
  if (test_and_clear_bit(UGC_NAME_TO_INDEX(name), group->acquiredbit)) {
    --group->num_acquired;
  } else {
    printk(KERN_DEBUG pr_fmt("Cannot release name %d; it is not acquired.\n"),
        UGC_NAME_TO_INDEX(name));
  }
}

// scales the value into the range of a __u32
__u32 normalize_value(__u32 value, __s32 minimum, __s32 maximum) {
  return (__u32)((__u64)((__s64)value - minimum) * (((__u64)1 << 32) - 1) /
      (__u64)((__s64)maximum - minimum));
}

struct ugc_device_group g_device_group = {0};
struct ugc_device g_devices[UGC_MAX_DEVICES];


static int ugc_connect_device(struct input_handler *handler, struct input_dev *dev,
    const struct input_device_id *id)
{
  struct input_handle *handle;
  int error;
  const char* bus_name;
  const char* device_name;
  handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
  if (!handle) {
    return -ENOMEM;
  }

  device_name = ugc_device_name_acquire(&g_device_group);
  if (!device_name) {
    printk(KERN_DEBUG pr_fmt("Device connected, but no names available.\n"));
    return 0;
  }
  handle->dev = dev;
  handle->handler = handler;
  handle->name = device_name;

  error = input_register_handle(handle);
  if (error)
    goto err_free_handle;

  error = input_open_device(handle);
  if (error)
    goto err_unregister_handle;

  g_devices[UGC_NAME_TO_INDEX(device_name)] = (struct ugc_device) {
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
  ugc_device_name_release(&g_device_group, handle->name);

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
  //printk(KERN_DEBUG pr_fmt("Event. Dev: %s, Type: %s[%d], Code: %s[%d], Value: %d\n"),
  //    dev_name(&handle->dev->dev), event_name, type, code_name, code, value);

  device = g_devices + UGC_NAME_TO_INDEX(handle->name);


  if (type == EV_REL || type == EV_KEY) {
    struct ugc_input this_input = {
      .type = type,
      .code = code,
      .positive = true  // for EV_KEY
    };
    if (type == EV_REL) {
        struct input_absinfo* absinfo = device->dev->absinfo + code;
        if (value < 0) {
          // TODO check absolute negative to ensure output is positive
          this_input.positive = false;
          this_input.value = normalize_value(0 - value, 0, -absinfo->minimum);
        } else {
          this_input.value = normalize_value(value, 0, absinfo->maximum);
        }
    } else {
      this_input.value = normalize_value(value, 0, 1);
    }

    if (device->config_state != READY &&
        this_input.value >= g_UGC_MIN_PRESSED_VALUE) {
      if (device->config_state == CONNECTED) {
        if (ugc_input_compare(&device->last_input, &this_input) == 0) {
          if (++device->count == 10) {
            device->config_state = CONFIGURING;
            device->count = 0;
          }
        } else {
          device->last_input = this_input;
          device->count = 1;
        }
      } else if (device->config_state == CONFIGURING) {
        const bool is_terminal = (
            ugc_input_compare(&device->last_input, &this_input) == 0);
        struct ugc_input *node = ugc_input_search(
            &device->input_code_to_index, &this_input);
        if (node || (device->count == 0 && is_terminal)) {
          // no double bindings, and first input can't be terminal
          return;
        }
        node = device->input_nodes + device->count;
        *node = this_input;
        node->value = device->count;
        // TODO output more
        printk(KERN_DEBUG pr_fmt("Adding button: %u, Code %u\n"),
            node->value, node->code);
        if (!ugc_input_insert(&device->input_code_to_index, node)) {
          printk(KERN_DEBUG pr_fmt("FAIL\n"));
        }
        ++device->count;
        if (is_terminal) {
          device->config_state = READY;
        }
      }
    } else if (device->config_state == READY) {
      struct ugc_input *node;
      node = ugc_input_search(&device->input_code_to_index, &this_input);
      if (node) {
        device->input_state[node->value] = this_input.value;
        printk(KERN_DEBUG pr_fmt("Button: %u, Value: %u\n"),
            node->value, this_input.value);
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
  r_int_config();
  return input_register_handler(&ugc_handler);
}

static void __exit ugc_exit(void)
{
  r_int_release();
  input_unregister_handler(&ugc_handler);
}

module_init(ugc_init);
module_exit(ugc_exit);
