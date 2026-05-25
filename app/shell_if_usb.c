#include "app_stm32.h"
#include "usbd_cdc_if.h"

#include "app_common.h"

#include "shell_if_usb.h"
#include "shell.h"

#include "event_list.h"
#include "event_dispatcher.h"
#include "circ_buffer.h"

#define TX_BUFFER_SIZE      1024

////////////////////////////////////////////////////////////////////////////////
//
// private variables
//
////////////////////////////////////////////////////////////////////////////////
static void shellif_usb_tx_usb(void);

static IRQn_Type              _irqn  = OTG_HS_IRQn;

static CircBuffer             _rx_cb;
static volatile uint8_t       _rx_buffer[CLI_RX_BUFFER_LENGTH];
static ShellIntf              _shell_usb_if;
static volatile bool          _initialized = false;

static volatile uint8_t       _tx_buffer[TX_BUFFER_SIZE];
static CircBuffer             _tx_cb;
static volatile uint8_t       _tx_in_prog = false;

////////////////////////////////////////////////////////////////////////////////
//
// circular buffer callback
//
////////////////////////////////////////////////////////////////////////////////
static void
shell_if_usb_enter_critical(CircBuffer* cb)
{
  NVIC_DisableIRQ(_irqn);
}

static void
shell_if_usb_leave_critical(CircBuffer* cb)
{
  NVIC_EnableIRQ(_irqn);
}

void
////////////////////////////////////////////////////////////////////////////////
//
// callbacks from USB CDC
//
////////////////////////////////////////////////////////////////////////////////
shell_if_usb_rx_notify(uint8_t* buf, uint32_t len)
{
  if(!_initialized) 
  {
    return;
  }

  //
  // runs in IRQ context
  //
  if(circ_buffer_enqueue(&_rx_cb, buf, len, true) == false)
  {
    // fucked up. overflow mostly.
    // do something here
  }

  event_set(1 << DISPATCH_EVENT_USB_CLI_RX);
}

void
shell_if_usb_tx_empty_notify(void)
{
  if(!_initialized) 
  {
    return;
  }

  //
  // runs in IRQ context
  //
  //event_set(1 << DISPATCH_EVENT_USB_CLI_TX);
  shellif_usb_tx_usb();
}

////////////////////////////////////////////////////////////////////////////////
//
// shell callback
//
////////////////////////////////////////////////////////////////////////////////
static bool
shell_if_usb_get_rx_data(ShellIntf* intf, uint8_t* data)
{
  if(circ_buffer_dequeue(&_rx_cb, data, 1, false) == false)
  {
    return false;
  }
  return true;
}

static void
shellif_usb_tx_usb(void)
{
  int num_bytes;
  extern uint8_t UserTxBufferFS[APP_TX_DATA_SIZE];

  if(circ_buffer_is_empty(&_tx_cb, true))
  {
    _tx_in_prog = false;
    return;
  }

  num_bytes = _tx_cb.num_bytes > APP_TX_DATA_SIZE ? APP_TX_DATA_SIZE : _tx_cb.num_bytes;
  circ_buffer_dequeue(&_tx_cb, UserTxBufferFS, num_bytes, true);

  //CDC_Transmit_FS(UserTxBufferFS, num_bytes);
  CDC_Transmit_FS(UserTxBufferFS, num_bytes);
  _tx_in_prog = true;
}

static bool
shell_if_usb_put_tx_data(ShellIntf* intf, uint8_t* data, uint16_t len)
{
  shell_if_usb_enter_critical(&_tx_cb);

  if(circ_buffer_enqueue(&_tx_cb, data, (uint8_t)len, true) == false)
  {
    //
    // no space left in queue. what should we do?
    //
    shell_if_usb_leave_critical(&_tx_cb);
    return false;
  }

  //
  // XXX
  // still locked
  //
  if(_tx_in_prog == false)
  {
    shellif_usb_tx_usb();
  }

  shell_if_usb_leave_critical(&_tx_cb);

  return true;
}

static void
shell_if_usb_event_handler(uint32_t event)
{
  shell_handle_rx(&_shell_usb_if);
}

////////////////////////////////////////////////////////////////////////////////
//
// public interfaces
//
////////////////////////////////////////////////////////////////////////////////
void
shell_if_usb_init(void)
{
  _shell_usb_if.cmd_buffer_ndx    = 0;
  _shell_usb_if.get_rx_data       = shell_if_usb_get_rx_data;
  _shell_usb_if.put_tx_data       = shell_if_usb_put_tx_data;

  INIT_LIST_HEAD(&_shell_usb_if.lh);

  circ_buffer_init(&_rx_cb, _rx_buffer, CLI_RX_BUFFER_LENGTH,
      shell_if_usb_enter_critical,
      shell_if_usb_leave_critical);

  circ_buffer_init(&_tx_cb, _tx_buffer, TX_BUFFER_SIZE,
      shell_if_usb_enter_critical,
      shell_if_usb_leave_critical);

  shell_if_register(&_shell_usb_if);

  event_register_handler(shell_if_usb_event_handler, DISPATCH_EVENT_USB_CLI_RX);

  _initialized = true;
}
