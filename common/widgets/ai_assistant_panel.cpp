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

    // Try to load the backend; if it's not running, fall back to the
    // built-in HTML chat UI.
    tryLoadBackend();
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
    LoadURL( m_backendURL );
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
    if( m_toolCallHandler )
    {
        m_toolCallHandler( this, aMessage );
    }
    else
    {
        wxLogTrace( wxS( "AiAssistant" ), wxS( "Tool call received (no handler): %s" ), aMessage );
    }
}
