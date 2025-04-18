/* Copyright (C) 2002-2005 RealVNC Ltd.  All Rights Reserved.
 * Copyright 2011-2019 Pierre Ossman for Cendio AB
 * 
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

// -=- SDisplay.cxx
//
// The SDisplay class encapsulates a particular system display.

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>

#include <core/LogWriter.h>

#include <rfb_win32/SDisplay.h>
#include <rfb_win32/Service.h>
#include <rfb_win32/TsSessions.h>
#include <rfb_win32/CleanDesktop.h>
#include <rfb_win32/CurrentUser.h>
#include <rfb_win32/MonitorInfo.h>
#include <rfb_win32/SDisplayCorePolling.h>
#include <rfb_win32/SDisplayCoreWMHooks.h>
#include <rfb/VNCServer.h>
#include <rfb/ledStates.h>


using namespace core;
using namespace rdr;
using namespace rfb;
using namespace rfb::win32;

static LogWriter vlog("SDisplay");

// - SDisplay-specific configuration options

IntParameter rfb::win32::SDisplay::updateMethod("UpdateMethod",
  "How to discover desktop updates; 0 - Polling, 1 - Application hooking, 2 - Driver hooking.",
  0, 0, 2);
BoolParameter rfb::win32::SDisplay::disableLocalInputs("DisableLocalInputs",
  "Disable local keyboard and pointer input while the server is in use", false);
EnumParameter rfb::win32::SDisplay::disconnectAction("DisconnectAction",
  "Action to perform when all clients have disconnected.  (None, Lock, Logoff)",
  {"None", "Lock", "Logoff"}, "None");
StringParameter displayDevice("DisplayDevice",
  "Display device name of the monitor to be remoted, or empty to export the whole desktop.", "");
BoolParameter rfb::win32::SDisplay::removeWallpaper("RemoveWallpaper",
  "Remove the desktop wallpaper when the server is in use.", false);
BoolParameter rfb::win32::SDisplay::disableEffects("DisableEffects",
  "Disable desktop user interface effects when the server is in use.", false);


//////////////////////////////////////////////////////////////////////////////
//
// SDisplay
//

// -=- Constructor/Destructor

SDisplay::SDisplay()
  : server(nullptr), pb(nullptr), device(nullptr),
    core(nullptr), ptr(nullptr), kbd(nullptr), clipboard(nullptr),
    inputs(nullptr), monitor(nullptr), cleanDesktop(nullptr), cursor(nullptr),
    statusLocation(nullptr), queryConnectionHandler(nullptr), ledState(0)
{
  updateEvent.h = CreateEvent(nullptr, TRUE, FALSE, nullptr);
  terminateEvent.h = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

SDisplay::~SDisplay()
{
  // XXX when the VNCServer has been deleted with clients active, stop()
  // doesn't get called - this ought to be fixed in VNCServerST.  In any event,
  // we should never call any methods on VNCServer once we're being deleted.
  // This is because it is supposed to be guaranteed that the SDesktop exists
  // throughout the lifetime of the VNCServer.  So if we're being deleted, then
  // the VNCServer ought not to exist and therefore we shouldn't invoke any
  // methods on it.  Setting server to zero here ensures that stop() doesn't
  // call setPixelBuffer(0) on the server.
  server = nullptr;
  if (core) stop();
}


// -=- SDesktop interface

void SDisplay::init(VNCServer* vs)
{
  server = vs;
}

void SDisplay::start()
{
  vlog.debug("Starting");

  // Try to make session zero the console session
  if (!inConsoleSession())
    setConsoleSession();

  // Start the SDisplay core
  startCore();

  vlog.debug("Started");

  if (statusLocation) *statusLocation = true;
}

void SDisplay::stop()
{
  vlog.debug("Stopping");

  // If we successfully start()ed then perform the DisconnectAction
  if (core) {
    CurrentUserToken cut;
    if (disconnectAction == "Logoff") {
      if (!cut.h)
        vlog.info("Ignoring DisconnectAction=Logoff - no current user");
      else
        ExitWindowsEx(EWX_LOGOFF, 0);
    } else if (disconnectAction == "Lock") {
      if (!cut.h) {
        vlog.info("Ignoring DisconnectAction=Lock - no current user");
      } else {
        LockWorkStation();
      }
    }
  }

  // Stop the SDisplayCore
  server->setPixelBuffer(nullptr);
  stopCore();

  vlog.debug("Stopped");

  if (statusLocation) *statusLocation = false;
}

void SDisplay::terminate()
{
  SetEvent(terminateEvent);
}


void SDisplay::queryConnection(network::Socket* sock,
                               const char* userName)
{
  assert(server != nullptr);

  if (queryConnectionHandler) {
    queryConnectionHandler->queryConnection(sock, userName);
    return;
  }

  server->approveConnection(sock, true);
}


void SDisplay::startCore() {

  // Currently, we just check whether we're in the console session, and
  //   fail if not
  if (!inConsoleSession())
    throw std::runtime_error("Console is not session zero - oreconnect to restore Console sessin");
  
  // Switch to the current input desktop
  if (rfb::win32::desktopChangeRequired()) {
    if (!rfb::win32::changeDesktop())
      throw std::runtime_error("Unable to switch into input desktop");
  }

  // Initialise the change tracker and clipper
  updates.clear();
  clipper.setUpdateTracker(server);

  // Create the framebuffer object
  recreatePixelBuffer(true);

  // Create the SDisplayCore
  updateMethod_ = updateMethod;
  int tryMethod = updateMethod_;
  while (!core) {
    try {
      if (tryMethod == 1)
        core = new SDisplayCoreWMHooks(this, &updates);
      else
        core = new SDisplayCorePolling(this, &updates);
      core->setScreenRect(screenRect);
    } catch (std::exception& e) {
      delete core; core = nullptr;
      if (tryMethod == 0)
        throw std::runtime_error("Unable to access desktop");
      tryMethod--;
      vlog.error("%s", e.what());
    }
  }
  vlog.info("Started %s", core->methodName());

  // Start display monitor, clipboard handler and input handlers
  monitor = new WMMonitor;
  monitor->setNotifier(this);
  clipboard = new Clipboard;
  clipboard->setNotifier(this);
  ptr = new SPointer;
  kbd = new SKeyboard;
  inputs = new WMBlockInput;
  cursor = new WMCursor;

  // Apply desktop optimisations
  cleanDesktop = new CleanDesktop;
  if (removeWallpaper)
    cleanDesktop->disableWallpaper();
  if (disableEffects)
    cleanDesktop->disableEffects();
  isWallpaperRemoved = removeWallpaper;
  areEffectsDisabled = disableEffects;

  checkLedState();
  if (server)
    server->setLEDState(ledState);
}

void SDisplay::stopCore() {
  if (core)
    vlog.info("Stopping %s", core->methodName());
  delete core; core = nullptr;
  delete pb; pb = nullptr;
  delete device; device = nullptr;
  delete monitor; monitor = nullptr;
  delete clipboard; clipboard = nullptr;
  delete inputs; inputs = nullptr;
  delete ptr; ptr = nullptr;
  delete kbd; kbd = nullptr;
  delete cleanDesktop; cleanDesktop = nullptr;
  delete cursor; cursor = nullptr;
  ResetEvent(updateEvent);
}


bool SDisplay::isRestartRequired() {
  // - We must restart the SDesktop if:
  // 1. We are no longer in the input desktop.
  // 2. The any setting has changed.

  // - Check that our session is the Console
  if (!inConsoleSession())
    return true;

  // - Check that we are in the input desktop
  if (rfb::win32::desktopChangeRequired())
    return true;

  // - Check that the update method setting hasn't changed
  //   NB: updateMethod reflects the *selected* update method, not
  //   necessarily the one in use, since we fall back to simpler
  //   methods if more advanced ones fail!
  if (updateMethod_ != updateMethod)
    return true;

  // - Check that the desktop optimisation settings haven't changed
  //   This isn't very efficient, but it shouldn't change very often!
  if ((isWallpaperRemoved != removeWallpaper) ||
      (areEffectsDisabled != disableEffects))
    return true;

  return false;
}


void SDisplay::restartCore() {
  vlog.info("Restarting");

  // Stop the existing Core  related resources
  stopCore();
  try {
    // Start a new Core if possible
    startCore();
    vlog.info("Restarted");
  } catch (std::exception& e) {
    // If startCore() fails then we MUST disconnect all clients,
    // to cause the server to stop() the desktop.
    // Otherwise, the SDesktop is in an inconsistent state
    // and the server will crash.
    server->closeClients(e.what());
  }
}


void SDisplay::handleClipboardRequest() {
  server->sendClipboardData(clipboard->getClipText().c_str());
}

void SDisplay::handleClipboardAnnounce(bool available) {
  // FIXME: Wait for an application to actually request it
  if (available)
    server->requestClipboard();
}

void SDisplay::handleClipboardData(const char* data) {
  clipboard->setClipText(data);
}


void SDisplay::pointerEvent(const Point& pos, uint16_t buttonmask) {
  if (pb->getRect().contains(pos)) {
    Point screenPos = pos.translate(screenRect.tl);
    // - Check that the SDesktop doesn't need restarting
    if (isRestartRequired())
      restartCore();
    if (ptr)
      ptr->pointerEvent(screenPos, buttonmask);
  }
}

void SDisplay::keyEvent(uint32_t keysym, uint32_t keycode, bool down) {
  // - Check that the SDesktop doesn't need restarting
  if (isRestartRequired())
    restartCore();
  if (kbd)
    kbd->keyEvent(keysym, keycode, down);
}

bool SDisplay::checkLedState() {
  unsigned state = 0;

  if (GetKeyState(VK_SCROLL) & 0x0001)
    state |= ledScrollLock;
  if (GetKeyState(VK_NUMLOCK) & 0x0001)
    state |= ledNumLock;
  if (GetKeyState(VK_CAPITAL) & 0x0001)
    state |= ledCapsLock;

  if (ledState != state) {
    ledState = state;
    return true;
  }

  return false;
}


void
SDisplay::notifyClipboardChanged(bool available) {
  vlog.debug("Clipboard text changed");
  if (server)
    server->announceClipboard(available);
}


void
SDisplay::notifyDisplayEvent(WMMonitor::Notifier::DisplayEventType evt) {
  switch (evt) {
  case WMMonitor::Notifier::DisplaySizeChanged:
    vlog.debug("Desktop size changed");
    recreatePixelBuffer();
    break;
  case WMMonitor::Notifier::DisplayPixelFormatChanged:
    vlog.debug("Desktop format changed");
    recreatePixelBuffer();
    break;
  default:
    vlog.error("Unknown display event received");
  }
}

void
SDisplay::processEvent(HANDLE event) {
  if (event == updateEvent) {
    vlog.write(120, "processEvent");
    ResetEvent(updateEvent);

    // - If the SDisplay isn't even started then quit now
    if (!core) {
      vlog.error("Not start()ed");
      return;
    }

    // - Ensure that the disableLocalInputs flag is respected
    inputs->blockInputs(disableLocalInputs);

    // - Only process updates if the server is ready
    if (server) {
      // - Check that the SDesktop doesn't need restarting
      if (isRestartRequired()) {
        restartCore();
        return;
      }

      // - Flush any updates from the core
      try {
        core->flushUpdates();
      } catch (std::exception& e) {
        vlog.error("%s", e.what());
        restartCore();
        return;
      }

      // Ensure the cursor is up to date
      WMCursor::Info info = cursor->getCursorInfo();
      if (old_cursor != info) {
        // Update the cursor shape if the visibility has changed
        bool set_cursor = info.visible != old_cursor.visible;
        // OR if the cursor is visible and the shape has changed.
        set_cursor |= info.visible && (old_cursor.cursor != info.cursor);

        // Update the cursor shape
        if (set_cursor)
          pb->setCursor(info.visible ? info.cursor : nullptr, server);

        // Update the cursor position
        // NB: First translate from Screen coordinates to Desktop
        Point desktopPos = info.position.translate(screenRect.tl.negate());
        server->setCursorPos(desktopPos, false);

        old_cursor = info;
      }

      // Flush any changes to the server
      flushChangeTracker();

      // Forward current LED state to the server
      if (checkLedState())
        server->setLEDState(ledState);
    }
    return;
  }
  throw std::runtime_error("No such event");
}


// -=- Protected methods

void
SDisplay::recreatePixelBuffer(bool force) {
  // Open the specified display device
  //   If no device is specified, open entire screen using GetDC().
  //   Opening the whole display with CreateDC doesn't work on multi-monitor
  //   systems for some reason.
  DeviceContext* new_device = nullptr;
  if (strlen(displayDevice) > 0) {
    vlog.info("Attaching to device %s", (const char*)displayDevice);
    new_device = new DeviceDC(displayDevice);
  }
  if (!new_device) {
    vlog.info("Attaching to virtual desktop");
    new_device = new WindowDC(nullptr);
  }

  // Get the coordinates of the specified dispay device
  Rect newScreenRect;
  if (strlen(displayDevice) > 0) {
    MonitorInfo info(displayDevice);
    newScreenRect = {info.rcMonitor.left, info.rcMonitor.top,
                     info.rcMonitor.right, info.rcMonitor.bottom};
  } else {
    newScreenRect = new_device->getClipBox();
  }

  // If nothing has changed & a recreate has not been forced, delete
  // the new device context and return
  if (pb && !force &&
    newScreenRect == screenRect &&
    new_device->getPF() == pb->getPF()) {
    delete new_device;
    return;
  }

  // Flush any existing changes to the server
  flushChangeTracker();

  // Delete the old pixelbuffer and device context
  vlog.debug("Deleting old pixel buffer & device");
  if (pb)
    delete pb;
  if (device)
    delete device;

  // Create a DeviceFrameBuffer attached to the new device
  vlog.debug("Creating pixel buffer");
  DeviceFrameBuffer* new_buffer = new DeviceFrameBuffer(*new_device);

  // Replace the old PixelBuffer
  screenRect = newScreenRect;
  pb = new_buffer;
  device = new_device;

  // Initialise the pixels
  pb->grabRegion(pb->getRect());

  // Prevent future grabRect operations from throwing exceptions
  pb->setIgnoreGrabErrors(true);

  // Update the clipping update tracker
  clipper.setClipRect(pb->getRect());

  // Inform the core of the changes
  if (core)
    core->setScreenRect(screenRect);

  // Inform the server of the changes
  if (server)
    server->setPixelBuffer(pb);
}

bool SDisplay::flushChangeTracker() {
  if (updates.is_empty())
    return false;

  vlog.write(120, "flushChangeTracker");

  // Translate the update coordinates from Screen coords to Desktop
  updates.translate(screenRect.tl.negate());

  // Clip the updates & flush them to the server
  updates.copyTo(&clipper);
  updates.clear();
  return true;
}
