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

// This file implements the AI tool handler for the schematic editor.
// It is compiled as part of the eeschema library because it needs access
// to eeschema internals (SCH_EDIT_FRAME, SCH_SYMBOL, etc.).
//
// The AI_ASSISTANT_PANEL (in common/) defines a callback-based tool call
// interface. This file provides the eeschema-specific implementation and
// a helper to wire it into the panel via SetToolCallHandler().

#include <widgets/ai_assistant_panel.h>
#include <wx/log.h>
#include <wx/translation.h>

#include <sch_edit_frame.h>
#include <sch_base_frame.h>
#include <sch_symbol.h>
#include <sch_screen.h>
#include <sch_commit.h>
#include <sch_draw_panel.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_no_connect.h>
#include <sch_text.h>
#include <sch_marker.h>
#include <lib_symbol.h>
#include <lib_id.h>
#include <schematic.h>
#include <sch_sheet_path.h>
#include <sch_reference_list.h>
#include <tool/tool_manager.h>
#include <gal/graphics_abstraction_layer.h>
#include <view/view.h>
#include <base_units.h>
#include <nlohmann/json.hpp>
#include <reporter.h>
#include <symbol.h>

#include <project.h>
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <pgm_base.h>
#include <erc/erc.h>

using json = nlohmann::json;


// Forward declarations
static wxString handleAddSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleSearchSymbols( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleSearchFootprints( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleGetSchematic( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleDeleteSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleSetProperty( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleDrawWire( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAddLabel( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAddJunction( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAddNoConnect( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAnnotate( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleRunErc( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleMoveSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleRotateSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAddText( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );


// Helper: find the SCH_EDIT_FRAME parent of the panel by walking up the
// wxWindow hierarchy.
static SCH_EDIT_FRAME* getSchEditFrame( wxWindow* aWindow )
{
    wxWindow* parent = aWindow->GetParent();

    while( parent )
    {
        if( SCH_EDIT_FRAME* frame = dynamic_cast<SCH_EDIT_FRAME*>( parent ) )
            return frame;

        parent = parent->GetParent();
    }

    return nullptr;
}


// ---------------------------------------------------------------------------
// Tool: add_symbol
//
// Adds a symbol to the current schematic sheet at the given position.
//
// Arguments (JSON):
//   library_name  — e.g. "Device:R"
//   x             — X position in millimetres
//   y             — Y position in millimetres
//   value         — (optional) value to set, e.g. "10k"
//   footprint     — (optional) footprint to set, e.g. "Resistor_SMD:R_0603"
//   reference     — (optional) reference designator, e.g. "R1"
//
// Returns (JSON):
//   { "status": "ok", "ref": "R1", "library": "Device:R",
//     "position": { "x": 25, "y": 25 } }
//   or { "error": "..." }
// ---------------------------------------------------------------------------
static wxString handleAddSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString libName = wxString::FromUTF8( aArgs.value( "library_name", "" ).c_str() );

    if( libName.IsEmpty() )
        return R"({ "error": "Missing 'library_name' argument" })";

    // Parse the library name (e.g. "Device:R")
    LIB_ID libId;
    libId.Parse( libName, true );

    if( !libId.IsValid() )
        return wxString::Format( R"({ "error": "Invalid library name: %s" })", libName );

    // Load the symbol from the library table
    LIB_SYMBOL* libSymbol = frame->GetLibSymbol( libId, true, false );

    if( !libSymbol )
        return wxString::Format( R"({ "error": "Symbol not found in library: %s" })", libName );

    // Get the current sheet path
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    // Build the PICKED_SYMBOL struct
    PICKED_SYMBOL picked;
    picked.LibId = libId;
    picked.Unit = 1;
    picked.Convert = 1;

    // Parse optional field overrides from the tool arguments
    wxString fieldValue    = wxString::FromUTF8( aArgs.value( "value", "" ).c_str() );
    wxString fieldFootprint = wxString::FromUTF8( aArgs.value( "footprint", "" ).c_str() );
    wxString fieldReference = wxString::FromUTF8( aArgs.value( "reference", "" ).c_str() );

    if( !fieldValue.IsEmpty() )
        picked.Fields.emplace_back( FIELD_T::VALUE, fieldValue );

    if( !fieldFootprint.IsEmpty() )
        picked.Fields.emplace_back( FIELD_T::FOOTPRINT, fieldFootprint );

    if( !fieldReference.IsEmpty() )
        picked.Fields.emplace_back( FIELD_T::REFERENCE, fieldReference );

    // Convert mm to KiCad internal units (schIUScale is 100 nm per IU)
    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );
    VECTOR2I position( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );

    // Create the new schematic symbol from the library symbol
    SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, &currentSheet, picked,
                                          position, &frame->Schematic() );

    if( !symbol )
        return R"({ "error": "Failed to create symbol" })";

    // Autoplace fields (reference, value, etc.)
    symbol->AutoplaceFields( nullptr, AUTOPLACE_AUTO );

    // Add the symbol to the screen
    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( symbol, screen );

    // Create a commit so the action is undoable
    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( symbol, screen );
    commit.Push( _( "Add Symbol (AI)" ) );

    // Refresh the canvas so the symbol appears immediately
    frame->GetCanvas()->GetView()->Update( symbol );
    frame->GetCanvas()->Refresh();

    // Return the reference designator
    wxString ref = symbol->GetRef( &currentSheet, true );

    json result;
    result["status"] = "ok";
    result["ref"] = std::string( ref.ToUTF8() );
    result["library"] = std::string( libName.ToUTF8() );
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: set_property
//
// Modifies a property on an existing symbol.
//
// Arguments (JSON):
//   ref      — reference designator, e.g. "R1"
//   property — "value", "footprint", or "reference"
//   value    — the new value
//
// Returns (JSON):
//   { "status": "ok", "ref": "R1", "property": "value", "new_value": "22k" }
// ---------------------------------------------------------------------------
static wxString handleSetProperty( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString ref = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    wxString property = wxString::FromUTF8( aArgs.value( "property", "" ).c_str() );
    wxString newValue = wxString::FromUTF8( aArgs.value( "value", "" ).c_str() );

    if( ref.IsEmpty() || property.IsEmpty() || newValue.IsEmpty() )
        return R"({ "error": "Missing 'ref', 'property', or 'value' argument" })";

    property.LowerCase();

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_SYMBOL* target = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &currentSheet, true ) == ref )
        {
            target = symbol;
            break;
        }
    }

    if( !target )
        return wxString::Format( R"({ "error": "Symbol not found: %s" })", ref );

    // Apply the property change
    if( property == "value" )
    {
        target->SetValueFieldText( newValue, &currentSheet );
    }
    else if( property == "footprint" )
    {
        target->SetFootprintFieldText( newValue );
    }
    else if( property == "reference" )
    {
        target->SetRef( &currentSheet, newValue );
    }
    else
    {
        return wxString::Format( R"({ "error": "Unknown property: %s. Use 'value', 'footprint', or 'reference'." })", property );
    }

    // Create undoable commit
    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Modified( target, screen );
    commit.Push( _( "Set Property (AI)" ) );

    // Refresh canvas
    frame->GetCanvas()->GetView()->Update( target );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["ref"] = std::string( ref.ToUTF8() );
    result["property"] = std::string( property.ToUTF8() );
    result["new_value"] = std::string( newValue.ToUTF8() );

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: search_symbols
//
// Searches KiCad's symbol library table for symbols matching a keyword.
// Returns the full registry of matching symbols with their library names,
// descriptions, and default values.
//
// Arguments (JSON):
//   query   — (optional) search keyword, e.g. "resistor", "capacitor", "R"
//             If empty, returns ALL symbols in ALL libraries.
//
// Returns (JSON):
//   { "count": 42, "symbols": [ { "library_name": "Device:R",
//     "name": "R", "description": "Resistor", "value": "R" }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleSearchSymbols( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString query = wxString::FromUTF8( aArgs.value( "query", "" ).c_str() );
    query.LowerCase();

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &frame->Prj() );

    if( !adapter )
        return R"({ "error": "Symbol library adapter not available" })";

    std::vector<wxString> libNames = adapter->GetLibraryNames();

    json symbolsArray = json::array();
    int count = 0;

    for( const wxString& libNickname : libNames )
    {
        std::vector<wxString> symNames = adapter->GetSymbolNames( libNickname );

        for( const wxString& symName : symNames )
        {
            wxString fullId = libNickname + wxS( ":" ) + symName;

            // Load the symbol to get its description (needed for search)
            LIB_SYMBOL* libSym = adapter->LoadSymbol( libNickname, symName );
            wxString description;
            wxString defaultValue;

            if( libSym )
            {
                description = libSym->GetDescription();
                defaultValue = libSym->GetValueField().GetText();
            }

            // If a query is provided, filter by name, library, AND description
            if( !query.IsEmpty() )
            {
                wxString lowerSym = symName;
                lowerSym.LowerCase();

                wxString lowerLib = libNickname;
                lowerLib.LowerCase();

                wxString lowerDesc = description;
                lowerDesc.LowerCase();

                if( !lowerSym.Contains( query ) && !lowerLib.Contains( query ) && !lowerDesc.Contains( query ) )
                    continue;
            }

            json entry;
            entry["library_name"] = std::string( fullId.ToUTF8() );
            entry["name"] = std::string( symName.ToUTF8() );
            entry["library"] = std::string( libNickname.ToUTF8() );
            entry["description"] = std::string( description.ToUTF8() );
            entry["default_value"] = std::string( defaultValue.ToUTF8() );

            symbolsArray.push_back( entry );
            count++;

            // Cap at 500 results to avoid massive responses
            if( count >= 500 )
                break;
        }

        if( count >= 500 )
            break;
    }

    json result;
    result["count"] = count;
    result["symbols"] = symbolsArray;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: search_footprints
//
// Searches KiCad's footprint library table for footprint libraries matching
// a keyword. Returns the list of available footprint libraries.
//
// Note: Individual footprint names within each library are not enumerated
// here because that requires pcbnew internals. The agent can construct
// footprint IDs as "LibraryNickname:FootprintName" (e.g.
// "Resistor_SMD:R_0603_1608Metric") using standard KiCad naming conventions.
//
// Arguments (JSON):
//   query   — (optional) search keyword, e.g. "Resistor", "Capacitor", "SOIC"
//             If empty, returns ALL footprint libraries.
//
// Returns (JSON):
//   { "count": 42, "libraries": [ { "name": "Resistor_SMD", ... }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleSearchFootprints( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString query = wxString::FromUTF8( aArgs.value( "query", "" ).c_str() );
    query.LowerCase();

    // Use the global library manager to get footprint library rows.
    // This avoids needing pcbnew headers — LIBRARY_MANAGER is in common/.
    LIBRARY_MANAGER& libMgr = Pgm().GetLibraryManager();
    std::vector<LIBRARY_TABLE_ROW*> rows = libMgr.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT );

    json libsArray = json::array();
    int count = 0;

    for( const LIBRARY_TABLE_ROW* row : rows )
    {
        wxString libName = row->Nickname();

        // If a query is provided, filter by it
        if( !query.IsEmpty() )
        {
            wxString lowerLib = libName;
            lowerLib.LowerCase();

            if( !lowerLib.Contains( query ) )
                continue;
        }

        json entry;
        entry["name"] = std::string( libName.ToUTF8() );
        entry["type"] = "footprint_library";

        libsArray.push_back( entry );
        count++;
    }

    json result;
    result["count"] = count;
    result["libraries"] = libsArray;
    result["note"] = "Use 'LibraryName:FootprintName' format for footprint IDs, e.g. 'Resistor_SMD:R_0603_1608Metric'";

    return wxString::FromUTF8( result.dump().c_str() );
}
// ---------------------------------------------------------------------------
// Tool: draw_wire
//
// Draws a wire between two points on the schematic.
// Wires connect pins and other wires to form electrical nets.
//
// Arguments (JSON):
//   x1, y1 — start point in millimetres
//   x2, y2 — end point in millimetres
//
// Returns (JSON):
//   { "status": "ok", "from": { "x": 25, "y": 25 }, "to": { "x": 50, "y": 25 } }
// ---------------------------------------------------------------------------
static wxString handleDrawWire( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    double x1 = aArgs.value( "x1", 0.0 );
    double y1 = aArgs.value( "y1", 0.0 );
    double x2 = aArgs.value( "x2", 0.0 );
    double y2 = aArgs.value( "y2", 0.0 );

    VECTOR2I start( schIUScale.mmToIU( x1 ), schIUScale.mmToIU( y1 ) );
    VECTOR2I end( schIUScale.mmToIU( x2 ), schIUScale.mmToIU( y2 ) );

    SCH_LINE* wire = new SCH_LINE( start, LAYER_WIRE );
    wire->SetEndPoint( end );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( wire, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( wire, screen );
    commit.Push( _( "Draw Wire (AI)" ) );

    frame->GetCanvas()->GetView()->Update( wire );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["from"]["x"] = x1;
    result["from"]["y"] = y1;
    result["to"]["x"] = x2;
    result["to"]["y"] = y2;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: add_label
//
// Adds a net label at a position on the schematic. Labels name a net so that
// wires with the same label name are electrically connected without needing
// a physical wire between them.
//
// Arguments (JSON):
//   x, y       — position in millimetres (where the label connects to a wire)
//   text       — the net name, e.g. "VCC", "SIGNAL_OUT"
//   label_type — (optional) "local" (default), "global", or "hierarchical"
//
// Returns (JSON):
//   { "status": "ok", "text": "VCC", "position": { "x": 100, "y": 50 } }
// ---------------------------------------------------------------------------
static wxString handleAddLabel( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );
    wxString text = wxString::FromUTF8( aArgs.value( "text", "" ).c_str() );
    wxString labelType = wxString::FromUTF8( aArgs.value( "label_type", "local" ).c_str() );
    labelType.LowerCase();

    if( text.IsEmpty() )
        return R"({ "error": "Missing 'text' argument" })";

    VECTOR2I pos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );

    SCH_LABEL_BASE* label = nullptr;

    if( labelType == "global" )
        label = new SCH_GLOBALLABEL( pos, text );
    else if( labelType == "hierarchical" )
        label = new SCH_HIERLABEL( pos, text );
    else
        label = new SCH_LABEL( pos, text );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( label, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( label, screen );
    commit.Push( _( "Add Label (AI)" ) );

    frame->GetCanvas()->GetView()->Update( label );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["text"] = std::string( text.ToUTF8() );
    result["label_type"] = std::string( labelType.ToUTF8() );
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: add_junction
//
// Adds a junction dot at a point on the schematic. Junctions are required
// when 3 or more wires meet at a single point to indicate they are connected.
//
// Arguments (JSON):
//   x, y — position in millimetres
//
// Returns (JSON):
//   { "status": "ok", "position": { "x": 50, "y": 50 } }
// ---------------------------------------------------------------------------
static wxString handleAddJunction( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );

    VECTOR2I pos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );

    SCH_JUNCTION* junction = new SCH_JUNCTION( pos );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( junction, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( junction, screen );
    commit.Push( _( "Add Junction (AI)" ) );

    frame->GetCanvas()->GetView()->Update( junction );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: add_no_connect
//
// Adds a no-connect flag at a position on the schematic. This marks a pin as
// intentionally unconnected, preventing ERC errors on unused pins.
//
// Arguments (JSON):
//   x, y — position in millimetres (should be at a pin location)
//
// Returns (JSON):
//   { "status": "ok", "position": { "x": 50, "y": 50 } }
// ---------------------------------------------------------------------------
static wxString handleAddNoConnect( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );

    VECTOR2I pos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );

    SCH_NO_CONNECT* nc = new SCH_NO_CONNECT( pos );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( nc, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( nc, screen );
    commit.Push( _( "Add No-Connect (AI)" ) );

    frame->GetCanvas()->GetView()->Update( nc );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: annotate
//
// Auto-numbers all unannotated symbols on the schematic. Assigns reference
// designators (R1, R2, C1, U1, etc.) to symbols that don't have them yet.
//
// Arguments (JSON):
//   (none — annotates the entire schematic incrementally)
//
// Returns (JSON):
//   { "status": "ok", "message": "Annotation complete" }
// ---------------------------------------------------------------------------
static wxString handleAnnotate( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    WX_STRING_REPORTER reporter;

    SCH_COMMIT commit( frame->GetToolManager() );

    frame->AnnotateSymbols( &commit, ANNOTATE_ALL, SORT_BY_X_POSITION,
                            INCREMENTAL_BY_REF, true, 100, false, false, false,
                            reporter, SYMBOL_FILTER_ALL );

    commit.Push( _( "Annotate Schematic (AI)" ) );

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["message"] = "Annotation complete";

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: run_erc
//
// Runs Electrical Rules Check on the schematic and returns a list of errors
// and warnings. Call this after adding components and wires to verify the
// schematic has no electrical issues.
//
// Arguments (JSON):
//   (none)
//
// Returns (JSON):
//   { "status": "ok", "error_count": 2, "warning_count": 1,
//     "markers": [ { "message": "...", "severity": "error", "x": 50, "y": 50 }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleRunErc( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCHEMATIC* sch = &frame->Schematic();

    if( !sch )
        return R"({ "error": "No schematic loaded" })";

    // Record exclusions and clear old markers
    sch->RecordERCExclusions();

    // Delete all existing ERC markers from all screens
    SCH_SCREENS screens( sch->Root() );
    screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, true );

    // Recalculate connections to build the connection graph
    SCH_COMMIT commit( frame->GetToolManager() );
    sch->RecalculateConnections( &commit, GLOBAL_CLEANUP, frame->GetToolManager() );
    commit.Push( _( "Recalculate (AI ERC)" ) );

    // Run ERC on the connection graph
    sch->ConnectionGraph()->RunERC();

    // Collect all ERC markers from all screens
    json markersArray = json::array();
    int errorCount = 0;
    int warningCount = 0;

    SCH_SCREEN* screen = screens.GetFirst();

    while( screen )
    {
        for( SCH_ITEM* item : screen->Items().OfType( SCH_MARKER_T ) )
        {
            SCH_MARKER* marker = static_cast<SCH_MARKER*>( item );

            SEVERITY severity = marker->GetSeverity();
            wxString severityStr;

            if( severity == RPT_SEVERITY_ERROR )
            {
                severityStr = wxS( "error" );
                errorCount++;
            }
            else if( severity == RPT_SEVERITY_WARNING )
            {
                severityStr = wxS( "warning" );
                warningCount++;
            }
            else
            {
                continue;
            }

            auto rcItem = marker->GetRCItem();
            wxString errorMsg;

            if( rcItem )
                errorMsg = rcItem->GetErrorMessage( true );
            else
                errorMsg = wxS( "Unknown ERC error" );

            VECTOR2I pos = marker->GetPosition();

            json entry;
            entry["message"] = std::string( errorMsg.ToUTF8() );
            entry["severity"] = std::string( severityStr.ToUTF8() );
            entry["x"] = schIUScale.IUTomm( pos.x );
            entry["y"] = schIUScale.IUTomm( pos.y );

            markersArray.push_back( entry );
        }

        screen = screens.GetNext();
    }

    // Refresh canvas to show markers
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["error_count"] = errorCount;
    result["warning_count"] = warningCount;
    result["markers"] = markersArray;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: move_symbol
//
// Moves a symbol to a new position on the schematic.
//
// Arguments (JSON):
//   ref — reference designator, e.g. "R1"
//   x   — new X position in millimetres
//   y   — new Y position in millimetres
//
// Returns (JSON):
//   { "status": "ok", "ref": "R1", "position": { "x": 100, "y": 50 } }
// ---------------------------------------------------------------------------
static wxString handleMoveSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString ref = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );

    if( ref.IsEmpty() )
        return R"({ "error": "Missing 'ref' argument" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_SYMBOL* target = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &currentSheet, true ) == ref )
        {
            target = symbol;
            break;
        }
    }

    if( !target )
        return wxString::Format( R"({ "error": "Symbol not found: %s" })", ref );

    VECTOR2I newPos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );
    target->SetPosition( newPos );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Modified( target, screen );
    commit.Push( _( "Move Symbol (AI)" ) );

    frame->GetCanvas()->GetView()->Update( target );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["ref"] = std::string( ref.ToUTF8() );
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: rotate_symbol
//
// Rotates a symbol 90 degrees counter-clockwise around its center.
//
// Arguments (JSON):
//   ref     — reference designator, e.g. "R1"
//   degrees — (optional) rotation in degrees: 90, 180, or 270 (default: 90)
//
// Returns (JSON):
//   { "status": "ok", "ref": "R1", "rotation": 90 }
// ---------------------------------------------------------------------------
static wxString handleRotateSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString ref = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    int degrees = aArgs.value( "degrees", 90 );

    // Normalize to 90/180/270
    degrees = ( ( degrees % 360 ) + 360 ) % 360;

    if( degrees == 0 )
        degrees = 90;

    if( ref.IsEmpty() )
        return R"({ "error": "Missing 'ref' argument" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_SYMBOL* target = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &currentSheet, true ) == ref )
        {
            target = symbol;
            break;
        }
    }

    if( !target )
        return wxString::Format( R"({ "error": "Symbol not found: %s" })", ref );

    VECTOR2I center = target->GetPosition();

    // Apply rotation the required number of 90-degree steps
    int steps = degrees / 90;

    for( int i = 0; i < steps; i++ )
        target->Rotate( center, true ); // true = counter-clockwise

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Modified( target, screen );
    commit.Push( _( "Rotate Symbol (AI)" ) );

    frame->GetCanvas()->GetView()->Update( target );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["ref"] = std::string( ref.ToUTF8() );
    result["rotation"] = degrees;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: add_text
//
// Adds a text annotation to the schematic. This is for documentation/notes
// only — it has no electrical meaning.
//
// Arguments (JSON):
//   x, y  — position in millimetres
//   text  — the text content
//   size  — (optional) font size in millimetres (default: 1.27mm = 50 mils)
//
// Returns (JSON):
//   { "status": "ok", "text": "Note: filter stage", "position": { "x": 50, "y": 50 } }
// ---------------------------------------------------------------------------
static wxString handleAddText( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );
    wxString text = wxString::FromUTF8( aArgs.value( "text", "" ).c_str() );
    double sizeMm = aArgs.value( "size", 1.27 );

    if( text.IsEmpty() )
        return R"({ "error": "Missing 'text' argument" })";

    VECTOR2I pos( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) );

    SCH_TEXT* textItem = new SCH_TEXT( pos, text, LAYER_NOTES );
    textItem->SetSchTextSize( schIUScale.mmToIU( sizeMm ) );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( textItem, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( textItem, screen );
    commit.Push( _( "Add Text (AI)" ) );

    frame->GetCanvas()->GetView()->Update( textItem );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["text"] = std::string( text.ToUTF8() );
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Main tool call dispatcher.
//
// This is set as the AI_ASSISTANT_PANEL's tool call handler via
// SetToolCallHandler() in SCH_EDIT_FRAME.
// ---------------------------------------------------------------------------
void handleSchToolCall( AI_ASSISTANT_PANEL* aPanel, const wxString& aMessage )
{
    json request;
    try
    {
        request = json::parse( std::string( aMessage.ToUTF8() ) );
    }
    catch( const json::parse_error& )
    {
        aPanel->RunScriptAsync(
            wxString::Format(
                "window.kicadAi && window.kicadAi.addMessage('Error: invalid JSON', 'system')" ) );
        return;
    }

    wxString toolName   = wxString::FromUTF8( request.value( "tool", "" ).c_str() );
    wxString requestId  = wxString::FromUTF8( request.value( "requestId", "" ).c_str() );
    json     args       = request.value( "args", json::object() );

    wxString result;

    if( toolName == "add_symbol" )
    {
        result = handleAddSymbol( aPanel, args );
    }
    else if( toolName == "search_symbols" )
    {
        result = handleSearchSymbols( aPanel, args );
    }
    else if( toolName == "search_footprints" )
    {
        result = handleSearchFootprints( aPanel, args );
    }
    else if( toolName == "get_schematic" )
    {
        result = handleGetSchematic( aPanel, args );
    }
    else if( toolName == "delete_symbol" )
    {
        result = handleDeleteSymbol( aPanel, args );
    }
    else if( toolName == "set_property" )
    {
        result = handleSetProperty( aPanel, args );
    }
    else if( toolName == "draw_wire" )
    {
        result = handleDrawWire( aPanel, args );
    }
    else if( toolName == "add_label" )
    {
        result = handleAddLabel( aPanel, args );
    }
    else if( toolName == "add_junction" )
    {
        result = handleAddJunction( aPanel, args );
    }
    else if( toolName == "add_no_connect" )
    {
        result = handleAddNoConnect( aPanel, args );
    }
    else if( toolName == "annotate" )
    {
        result = handleAnnotate( aPanel, args );
    }
    else if( toolName == "run_erc" )
    {
        result = handleRunErc( aPanel, args );
    }
    else if( toolName == "move_symbol" )
    {
        result = handleMoveSymbol( aPanel, args );
    }
    else if( toolName == "rotate_symbol" )
    {
        result = handleRotateSymbol( aPanel, args );
    }
    else if( toolName == "add_text" )
    {
        result = handleAddText( aPanel, args );
    }
    else
    {
        result = wxString::Format( R"({ "error": "Unknown tool: %s" })", toolName );
    }

    // Send the result back to the webview
    json response;
    response["requestId"] = std::string( requestId.ToUTF8() );

    try
    {
        response["result"] = json::parse( std::string( result.ToUTF8() ) );
    }
    catch( ... )
    {
        response["result"] = { { "error", std::string( result.ToUTF8() ) } };
    }

    wxString responseStr = wxString::FromUTF8( response.dump().c_str() );

    wxString script = wxString::Format(
        "window.kicadAi && window.kicadAi.onToolResult && window.kicadAi.onToolResult(%s)",
        responseStr );

    aPanel->RunScriptAsync( script );
}


// ---------------------------------------------------------------------------
// Tool: get_schematic
//
// Returns all symbols currently on the schematic with their properties.
//
// Returns (JSON):
//   { "count": 3, "components": [
//     { "ref": "R1", "value": "10k", "library": "Device:R",
//       "footprint": "Resistor_THT:...", "x": 50, "y": 50 }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleGetSchematic( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    json componentsArray = json::array();
    int count = 0;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        wxString ref = symbol->GetRef( &currentSheet, true );
        wxString value = symbol->GetValue( true, &currentSheet, false );
        wxString libId = symbol->GetLibId().Format();
        wxString footprint = symbol->GetFootprintFieldText( true, &currentSheet, false );
        VECTOR2I pos = symbol->GetPosition();

        json entry;
        entry["ref"] = std::string( ref.ToUTF8() );
        entry["value"] = std::string( value.ToUTF8() );
        entry["library"] = std::string( libId.ToUTF8() );
        entry["footprint"] = std::string( footprint.ToUTF8() );
        entry["x"] = schIUScale.IUTomm( pos.x );
        entry["y"] = schIUScale.IUTomm( pos.y );

        componentsArray.push_back( entry );
        count++;
    }

    json result;
    result["count"] = count;
    result["components"] = componentsArray;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: delete_symbol
//
// Deletes a symbol from the schematic by reference designator.
//
// Arguments (JSON):
//   ref — reference designator, e.g. "R1"
//
// Returns (JSON):
//   { "status": "ok", "deleted": "R1" }
//   or { "error": "Symbol not found: R1" }
// ---------------------------------------------------------------------------
static wxString handleDeleteSymbol( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString refToDelete = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );

    if( refToDelete.IsEmpty() )
        return R"({ "error": "Missing 'ref' argument" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_SYMBOL* target = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        if( symbol->GetRef( &currentSheet, true ) == refToDelete )
        {
            target = symbol;
            break;
        }
    }

    if( !target )
        return wxString::Format( R"({ "error": "Symbol not found: %s" })", refToDelete );

    // Remove from screen and create undoable commit
    frame->RemoveFromScreen( target, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Removed( target, screen );
    commit.Push( _( "Delete Symbol (AI)" ) );

    // Refresh canvas
    frame->GetCanvas()->GetView()->Remove( target );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["deleted"] = std::string( refToDelete.ToUTF8() );

    return wxString::FromUTF8( result.dump().c_str() );
}
