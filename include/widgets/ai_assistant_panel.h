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
     * Call this when the backend server is running and you want the full chat UI.
     */
    void LoadFromURL( const wxString& url );

private:
    void setupMessageHandlers();
    void loadDefaultPage();
    void onToolCall( const wxString& aMessage );

    wxString m_backendURL;
};

#endif // AI_ASSISTANT_PANEL_H
