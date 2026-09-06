/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <widgets/ai_assistant_panel.h>
#include <wx/log.h>
#include <wx/translation.h>
#include <wx/socket.h>
#include <wx/sckaddr.h>

// Default backend URL.
static const wxString DEFAULT_BACKEND_URL = wxS( "http://localhost:9531" );

// How long to wait for the backend page before falling back to the built-in UI.
static const int BACKEND_LOAD_TIMEOUT_MS = 3000;

// How often to re-check for the backend while showing the built-in UI, so that
// starting the backend AFTER KiCad picks it up without restarting the editor.
static const int BACKEND_RETRY_MS = 5000;

// ---------------------------------------------------------------------------
// Built-in HTML chat UI (shown when no backend is running)
// ---------------------------------------------------------------------------
static const wxString BUILTIN_CHAT_HTML = wxString::FromUTF8(
R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    background: #1e1e1e;
    color: #d4d4d4;
    display: flex;
    flex-direction: column;
    height: 100vh;
    overflow: hidden;
  }
  #header {
    padding: 12px 16px;
    background: #252526;
    border-bottom: 1px solid #3c3c3c;
    font-size: 14px;
    font-weight: 600;
    color: #cccccc;
    display: flex;
    align-items: center;
    gap: 8px;
    flex-shrink: 0;
  }
  #header .dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    background: #f0ad4e;
  }
  #header .dot.connected { background: #5cb85c; }
  #messages {
    flex: 1;
    overflow-y: auto;
    padding: 16px;
    display: flex;
    flex-direction: column;
    gap: 12px;
  }
  .msg {
    max-width: 90%;
    padding: 10px 14px;
    border-radius: 8px;
    font-size: 13px;
    line-height: 1.5;
    word-wrap: break-word;
  }
  .msg.user {
    align-self: flex-end;
    background: #0e639c;
    color: #ffffff;
  }
  .msg.assistant {
    align-self: flex-start;
    background: #2d2d30;
    color: #d4d4d4;
    border: 1px solid #3c3c3c;
  }
  .msg.system {
    align-self: center;
    background: transparent;
    color: #888;
    font-size: 12px;
    font-style: italic;
    text-align: center;
    max-width: 100%;
  }
  #input-area {
    padding: 12px 16px;
    background: #252526;
    border-top: 1px solid #3c3c3c;
    display: flex;
    gap: 8px;
    flex-shrink: 0;
  }
  #msg-input {
    flex: 1;
    padding: 10px 12px;
    background: #3c3c3c;
    border: 1px solid #555;
    border-radius: 6px;
    color: #d4d4d4;
    font-size: 13px;
    font-family: inherit;
    outline: none;
  }
  #msg-input:focus { border-color: #0e639c; }
  #send-btn {
    padding: 10px 20px;
    background: #0e639c;
    border: none;
    border-radius: 6px;
    color: #fff;
    font-size: 13px;
    font-weight: 500;
    cursor: pointer;
    white-space: nowrap;
  }
  #send-btn:hover { background: #1177bb; }
  #send-btn:disabled { background: #555; cursor: default; }
  #status {
    padding: 6px 16px;
    background: #252526;
    border-top: 1px solid #3c3c3c;
    font-size: 11px;
    color: #888;
    text-align: center;
    flex-shrink: 0;
  }
  #messages::-webkit-scrollbar { width: 6px; }
  #messages::-webkit-scrollbar-track { background: #1e1e1e; }
  #messages::-webkit-scrollbar-thumb { background: #555; border-radius: 3px; }
</style>
</head>
<body>
  <div id="header">
    <span class="dot" id="status-dot"></span>
    <span>KiCad AI Assistant</span>
  </div>
  <div id="messages">
    <div class="msg system">No backend connected. Start your backend server to enable AI features.</div>
  </div>
  <div id="input-area">
    <input type="text" id="msg-input" placeholder="Type a message..." disabled />
    <button id="send-btn" disabled>Send</button>
  </div>
  <div id="status">Backend: not connected (localhost:9531)</div>
  <script>
    window.kicadAi = {
      enabled: false,
      requestCounter: 0,
      pendingRequests: {},
      enable: function() {
        window.kicadAi.enabled = true;
        document.getElementById('msg-input').disabled = false;
        document.getElementById('send-btn').disabled = false;
        document.getElementById('status-dot').classList.add('connected');
        document.getElementById('status').textContent = 'Backend: connected';
        document.getElementById('msg-input').focus();
      },
      addMessage: function(text, role) {
        var msgs = document.getElementById('messages');
        var div = document.createElement('div');
        div.className = 'msg ' + (role || 'assistant');
        div.textContent = text;
        msgs.appendChild(div);
        msgs.scrollTop = msgs.scrollHeight;
      },
      clearMessages: function() {
        document.getElementById('messages').innerHTML = '';
      },
      // Send a tool call to C++ via the webview message handler.
      // Returns a Promise that resolves with the result.
      callTool: function(toolName, args) {
        var requestId = 'req_' + (++window.kicadAi.requestCounter);
        var msg = JSON.stringify({
          tool: toolName,
          args: args || {},
          requestId: requestId
        });
        // Send to C++ via the registered webview message handler.
        // On macOS (WKWebView) this is window.webkit.messageHandlers.tool_call
        if( window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.tool_call ) {
          window.webkit.messageHandlers.tool_call.postMessage(msg);
        }
        return new Promise(function(resolve, reject) {
          window.kicadAi.pendingRequests[requestId] = { resolve: resolve, reject: reject };
        });
      },
      // Called from C++ (via RunScriptAsync) when a tool result is ready.
      onToolResult: function(response) {
        var requestId = response.requestId;
        var pending = window.kicadAi.pendingRequests[requestId];
        if (pending) {
          delete window.kicadAi.pendingRequests[requestId];
          if (response.result && response.result.error) {
            pending.reject(response.result.error);
          } else {
            pending.resolve(response.result);
          }
        }
      }
    };
    document.getElementById('msg-input').addEventListener('keydown', function(e) {
      if (e.key === 'Enter' && !e.shiftKey) {
        e.preventDefault();
        if (!document.getElementById('send-btn').disabled) {
          document.getElementById('send-btn').click();
        }
      }
    });
  </script>
</body>
</html>)HTML" );


AI_ASSISTANT_PANEL::AI_ASSISTANT_PANEL( wxWindow* parent, wxWindowID id,
                                        const wxPoint& pos, const wxSize& size ) :
    WEBVIEW_PANEL( parent, id, pos, size )
{
    m_backendURL = DEFAULT_BACKEND_URL;

    // Allow localhost URLs to load inside the webview (don't redirect to
    // the system browser).
    SetHandleExternalLinks( true );

    // Register the tool_call message handler BEFORE loading any page.
    // DoInitHandlers() runs when the page finishes loading, and it iterates
    // m_msgHandlers — so the handler must already be in the map by then.
    setupMessageHandlers();

    // Bind the page-loaded event so DoInitHandlers() actually runs.
    // Without this, OnWebViewLoaded never fires and AddScriptMessageHandler
    // is never called on the WKWebView.
    BindLoadedEvent();

    // Set up the retry timer — if the backend URL fails to load within
    // 3 seconds, fall back to the built-in HTML so the panel isn't white.
    m_retryTimer.SetOwner( this );
    m_retryTimer.Bind( wxEVT_TIMER, [this]( wxTimerEvent& )
    {
        onBackendLoadTimeout();
    } );

    // Track whether the page ever finished loading, so the timeout below can
    // tell "still loading" from "loaded fine" from "failed".
    Bind( wxEVT_WEBVIEW_LOADED,
          [this]( wxWebViewEvent& aEvt )
          {
              m_pageLoaded = true;
              aEvt.Skip();
          } );

    // Paint the built-in UI immediately, before anything is loaded over the
    // network. A wxWebView with no page is a stark white rectangle, and that is
    // what the panel showed for the whole time the backend load was in flight or
    // whenever it failed in a way that set neither the loaded nor the error flag.
    // Starting from the built-in page means the worst case is a readable
    // "no backend connected" panel rather than a blank one.
    loadDefaultPage();

    // Start the backend load from the event loop, NOT from this constructor.
    //
    // On localhost the page loads almost instantly, so calling tryLoadBackend()
    // inline meant wxEVT_WEBVIEW_LOADED could fire while the parent editor frame
    // was still being constructed — i.e. from inside a nested event loop. That
    // reaches DoInitHandlers() -> AddScriptMessageHandler(), which internally
    // runs script and yields the event loop again. WebKit's JSC cannot take that
    // reentrant yield and throws, and the exception escapes frame construction as
    // "Unhandled exception of unknown type", killing the editor before it opens.
    //
    // It only reproduced when the backend was actually up: with nothing serving
    // port 9531 the load never completes, the event never fires, and the editor
    // opens fine. Deferring the load lets the frame finish building first.
    CallAfter( [this]() { tryLoadBackend(); } );
}


AI_ASSISTANT_PANEL::~AI_ASSISTANT_PANEL()
{
}


void AI_ASSISTANT_PANEL::LoadFromURL( const wxString& url )
{
    m_backendURL = url;
    LoadURL( url );
}


void AI_ASSISTANT_PANEL::loadDefaultPage()
{
    SetPage( BUILTIN_CHAT_HTML );
}


void AI_ASSISTANT_PANEL::tryLoadBackend()
{
    // Always load the backend URL directly. The webview's JavaScript will
    // handle the WebSocket connection and show a "connecting..." state.
    // If the backend isn't running, the web UI shows a reconnect message
    // and retries automatically.
    //
    // We previously tried a socket check here, but it was unreliable on
    // macOS (IPv6 vs IPv4 mismatch with wxIPV4address).
    m_backendLoaded = true;
    m_fellBackToDefault = false;

    // Reset before each attempt. Otherwise the flag left set by the built-in
    // fallback page would make the next attempt look like it succeeded.
    m_pageLoaded = false;

    LoadURL( m_backendURL );

    m_retryTimer.StartOnce( BACKEND_LOAD_TIMEOUT_MS );
}


void AI_ASSISTANT_PANEL::onBackendLoadTimeout()
{
    // This timer does double duty. While the built-in page is showing it is a
    // retry tick: check whether the backend has come up since last time.
    if( m_fellBackToDefault )
    {
        tryLoadBackend();
        return;
    }

    // Otherwise it is the load deadline for a backend attempt.
    if( m_pageLoaded && !HasLoadError() )
        return; // backend page is up

    // Fall back if the page did not come up — either it reported an error, or it
    // simply never finished loading. Checking only HasLoadError() left the panel
    // blank whenever a load failed silently.
    wxLogTrace( wxS( "AiAssistant" ), wxS( "Backend load failed — showing built-in HTML" ) );
    m_fellBackToDefault = true;
    loadDefaultPage();

    // Keep looking for the backend. This removes the old requirement that the
    // backend must be started BEFORE KiCad — start it whenever, and the panel
    // switches over on its own.
    m_retryTimer.StartOnce( BACKEND_RETRY_MS );
}


void AI_ASSISTANT_PANEL::setupMessageHandlers()
{
    AddMessageHandler( wxS( "tool_call" ),
        [this]( const wxString& aMessage )
        {
            onToolCall( aMessage );
        } );
}


void AI_ASSISTANT_PANEL::onToolCall( const wxString& aMessage )
{
    if( !m_toolCallHandler )
    {
        wxLogTrace( wxS( "AiAssistant" ), wxS( "Tool call received (no handler): %s" ), aMessage );
        return;
    }

    // Run the tool from the event loop rather than inline.
    //
    // This function is called from inside a WKWebView script message handler.
    // Executing a tool inline means the whole tool -- library reads, ERC,
    // connection graph rebuilds -- runs while WebKit is still waiting for its
    // message callback to return, which blocks the webview and keeps the main
    // thread from processing paint or focus events. The result is an app that
    // appears hung and cannot be switched to.
    //
    // CallAfter() lets the message handler return immediately and runs the tool
    // on the next event loop iteration instead. The reply path is already
    // asynchronous (RunScriptAsync), so nothing downstream needs to change.
    wxString message = aMessage;

    CallAfter( [this, message]()
    {
        if( m_toolCallHandler )
            m_toolCallHandler( this, message );
    } );
}
