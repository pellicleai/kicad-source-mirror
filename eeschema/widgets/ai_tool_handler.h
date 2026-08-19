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

#ifndef AI_TOOL_HANDLER_H
#define AI_TOOL_HANDLER_H

#include <widgets/ai_assistant_panel.h>

class AI_ASSISTANT_PANEL;

/**
 * Eeschema-specific tool call dispatcher.
 *
 * Handles tool calls from the AI assistant panel by dispatching them to
 * the appropriate eeschema internal functions (add_symbol, draw_wire, etc.).
 *
 * Call this from SCH_EDIT_FRAME to wire the panel's tool call callback:
 *   m_aiAssistantPanel->SetToolCallHandler( &handleSchToolCall );
 */
void handleSchToolCall( AI_ASSISTANT_PANEL* aPanel, const wxString& aMessage );

#endif // AI_TOOL_HANDLER_H
