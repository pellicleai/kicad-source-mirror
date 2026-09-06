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

#ifndef AI_ASSISTANT_PANEL_H
#define AI_ASSISTANT_PANEL_H

#include <widgets/webview_panel.h>
#include <functional>
#include <wx/timer.h>

/**
 * A dockable sidebar panel that hosts an AI assistant chat interface.
 *
 * The panel embeds a #WEBVIEW_PANEL (a wxWebView wrapper). By default it
 * renders a built-in HTML chat UI so the panel is always functional even
 * when no backend server is running. When a backend is available, call
 * #LoadFromURL to switch the webview to the live backend URL.
 *
 * Communication:
 *  - Web UI -> C++: via JavaScript message handlers registered on the webview.
 *  - C++ -> Web UI: via RunScriptAsync() calls.
 */
class AI_ASSISTANT_PANEL : public WEBVIEW_PANEL
{
public:
    /// Callback type for tool calls. Takes the JSON message string.
    using TOOL_CALL_HANDLER = std::function<void( AI_ASSISTANT_PANEL*, const wxString& )>;

    explicit AI_ASSISTANT_PANEL( wxWindow* parent, wxWindowID id = wxID_ANY,
                                 const wxPoint& pos = wxDefaultPosition,
                                 const wxSize& size = wxDefaultSize );

    ~AI_ASSISTANT_PANEL() override;

    /**
     * The AUI pane name used to register this panel with the frame's AUI manager.
     */
    static const wxString PaneName() { return wxS( "AiAssistant" ); }

    /**
     * Switch the webview to load from a backend URL instead of the built-in HTML.
     */
    void LoadFromURL( const wxString& url );

    /**
     * Set the tool call handler. This is called when a tool call message
     * arrives from the webview. The handler is set by the editor frame
     * (e.g. SCH_EDIT_FRAME) to dispatch tool calls to KiCad's internal
     * functions.
     */
    void SetToolCallHandler( TOOL_CALL_HANDLER aHandler ) { m_toolCallHandler = std::move( aHandler ); }

private:
    void setupMessageHandlers();
    void loadDefaultPage();
    void tryLoadBackend();
    void onBackendLoadTimeout();   ///< Called by timer — falls back to built-in HTML
    void onToolCall( const wxString& aMessage );

    wxString           m_backendURL;
    bool               m_backendLoaded = false;
    bool               m_fellBackToDefault = false;  ///< True if we loaded built-in HTML because backend was down
    bool               m_pageLoaded = false;         ///< True once a page has finished loading in the webview
    TOOL_CALL_HANDLER  m_toolCallHandler;
    wxTimer            m_retryTimer;  ///< Retries loading the backend URL
};

#endif // AI_ASSISTANT_PANEL_H
