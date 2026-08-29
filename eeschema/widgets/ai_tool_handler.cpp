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
#include <tools/sch_line_wire_bus_tool.h>

using json = nlohmann::json;


// ---------------------------------------------------------------------------
// Helper: snap a position to the KiCad eeschema grid (50 mils = 1.27mm).
// This ensures wires and labels land exactly on pin positions, which are
// always on the grid. Without this, fractional mm coordinates from the LLM
// produce wire endpoints that don't connect to pins.
// ---------------------------------------------------------------------------
static VECTOR2I snapToGrid( const VECTOR2I& aPos )
{
    // 50 mils in KiCad internal units (1 mil = 25.4 um = 25400 nm = 254 IU)
    // 50 mils = 1.27mm = 1270000 nm = 12700 IU
    const int grid = schIUScale.mmToIU( 1.27 );

    VECTOR2I snapped;
    snapped.x = KiROUND( (double) aPos.x / grid ) * grid;
    snapped.y = KiROUND( (double) aPos.y / grid ) * grid;
    return snapped;
}

// Overload for mm input
static VECTOR2I mmToGrid( double xMm, double yMm )
{
    return snapToGrid( VECTOR2I( schIUScale.mmToIU( xMm ), schIUScale.mmToIU( yMm ) ) );
}


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
static wxString handleClearSchematic( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleGetPinPositions( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleConnectPins( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleConnectLabel( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );


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

    // Convert mm to KiCad internal units and snap to grid (50 mils = 1.27mm)
    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );
    VECTOR2I position = mmToGrid( xMm, yMm );

    // Create the new schematic symbol from the library symbol
    SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, &currentSheet, picked,
                                          position, &frame->Schematic() );

    if( !symbol )
        return R"({ "error": "Failed to create symbol" })";

    // Explicitly set the value field — the PICKED_SYMBOL constructor path
    // does not always propagate the value reliably, so we set it again here
    // to guarantee the component shows the correct value (e.g. "100k").
    if( !fieldValue.IsEmpty() )
        symbol->SetValueFieldText( fieldValue, &currentSheet );

    if( !fieldFootprint.IsEmpty() )
        symbol->SetFootprintFieldText( fieldFootprint );

    if( !fieldReference.IsEmpty() )
        symbol->SetRef( &currentSheet, fieldReference );

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
    wxString retValue = symbol->GetValue( true, &currentSheet, false );
    wxString retFootprint = symbol->GetFootprintFieldText( true, &currentSheet, false );

    json result;
    result["status"] = "ok";
    result["ref"] = std::string( ref.ToUTF8() );
    result["library"] = std::string( libName.ToUTF8() );
    result["value"] = std::string( retValue.ToUTF8() );
    result["footprint"] = std::string( retFootprint.ToUTF8() );
    result["position"]["x"] = xMm;
    result["position"]["y"] = yMm;

    return wxString::FromUTF8( result.dump().c_str() );
}

// ---------------------------------------------------------------------------
// Tool: get_pin_positions
//
// Returns the physical positions of all pins for a given symbol.
// This is essential for wiring — the LLM needs to know exactly where
// each pin is on the sheet to draw wires to the correct location.
//
// Arguments (JSON):
//   ref — reference designator, e.g. "U1". If empty, returns pins for ALL symbols.
//
// Returns (JSON):
//   { "status": "ok", "pins": [ { "ref": "U1", "name": "VCC", "number": "8", "x": 120.0, "y": 75.0 }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleGetPinPositions( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    wxString refFilter = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );

    json pinsArray = json::array();
    int count = 0;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
        wxString ref = symbol->GetRef( &currentSheet, true );

        // If a ref filter is given, only return pins for that symbol
        if( !refFilter.IsEmpty() && ref != refFilter )
            continue;

        bool isMulti = symbol->IsMultiUnit();
        std::vector<SCH_PIN*> pins = symbol->GetPins( &currentSheet );

        for( SCH_PIN* pin : pins )
        {
            VECTOR2I pinPos = pin->GetPosition();

            json pinEntry;
            pinEntry["ref"] = std::string( ref.ToUTF8() );

            // For multi-unit symbols (e.g. dual op-amps), include the
            // sub-unit ref so the LLM knows which sub-unit to use in
            // connect_pins and add_no_connect calls.  Sub-unit refs are
            // the parent ref + a letter suffix (U1A, U1B, etc.).
            if( isMulti )
            {
                int unit = pin->GetUnit();
                if( unit > 0 && unit <= 26 )
                {
                    wxString subRef = ref + wxChar( 'A' + unit - 1 );
                    pinEntry["sub_ref"] = std::string( subRef.ToUTF8() );
                }
            }

            pinEntry["name"] = std::string( pin->GetName().ToUTF8() );
            pinEntry["number"] = std::string( pin->GetNumber().ToUTF8() );
            pinEntry["x"] = schIUScale.IUTomm( pinPos.x );
            pinEntry["y"] = schIUScale.IUTomm( pinPos.y );
            pinsArray.push_back( pinEntry );
            count++;
        }
    }

    json result;
    result["status"] = "ok";
    result["count"] = count;
    result["pins"] = pinsArray;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Helper: extract the base reference from a possibly sub-unit reference.
// "U1A" → "U1", "R1" → "R1". Also returns the unit letter (0 if none).
// ---------------------------------------------------------------------------
static wxString extractBaseRef( const wxString& aRef, int* aUnitLetter = nullptr )
{
    if( aUnitLetter )
        *aUnitLetter = 0;

    if( aRef.Length() > 1 )
    {
        wxChar lastChar = aRef.Last();

        if( lastChar >= 'A' && lastChar <= 'Z' )
        {
            // Check if the character before the letter is a digit
            // (so "R1A" → base "R1", but "USA" → not a sub-unit ref)
            wxChar beforeLast = aRef[aRef.Length() - 2];
            if( beforeLast >= '0' && beforeLast <= '9' )
            {
                if( aUnitLetter )
                    *aUnitLetter = lastChar - 'A' + 1;
                return aRef.Left( aRef.Length() - 1 );
            }
        }
    }

    return aRef;
}

// Helper: find a specific pin on a symbol by reference + pin number
// ---------------------------------------------------------------------------
static SCH_PIN* findPinByRef( SCH_SCREEN* aScreen, SCH_SHEET_PATH& aSheet,
                               const wxString& aRef, const wxString& aPinNumber )
{
    // Handle multi-unit sub-refs (e.g. "U1A" → base "U1", unit 1)
    // KiCad stores multi-unit symbols as separate SCH_SYMBOL instances:
    // U1A has gate A pins, U1B has gate B pins, U1C has power pins, etc.
    // When the LLM says "U1A pin 7" (GND), pin 7 is on the power unit, not unit A.
    // So we need to search ALL sibling units (U1A, U1B, U1C...) for the pin.
    int     unitLetter = 0;
    wxString baseRef = extractBaseRef( aRef, &unitLetter );

    // Build the set of refs to search: if aRef is "U1A", search "U1A", "U1B", "U1C", etc.
    // If aRef is "U1" (no suffix), search "U1" and "U1A", "U1B", etc.
    wxArrayString refsToSearch;
    refsToSearch.Add( aRef );  // Always try the exact ref first

    if( unitLetter > 0 )
    {
        // aRef is like "U1A" — also search the base ref and all sibling units
        refsToSearch.Add( baseRef );
        for( char c = 'A'; c <= 'Z'; c++ )
        {
            wxString sibling = baseRef + c;
            if( sibling != aRef )
                refsToSearch.Add( sibling );
        }
    }
    else
    {
        // aRef is like "U1" — also search U1A, U1B, etc.
        for( char c = 'A'; c <= 'Z'; c++ )
        {
            refsToSearch.Add( baseRef + c );
        }
    }

    // Pass 1: match by pin number across all matching refs
    for( const wxString& searchRef : refsToSearch )
    {
        for( SCH_ITEM* item : aScreen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->GetRef( &aSheet, true ) != searchRef )
                continue;

            for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
            {
                if( pin->GetNumber() == aPinNumber )
                    return pin;
            }
        }
    }

    // Pass 2: match by pin name (e.g. "VBUS", "GND", "Shield")
    for( const wxString& searchRef : refsToSearch )
    {
        for( SCH_ITEM* item : aScreen->Items().OfType( SCH_SYMBOL_T ) )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->GetRef( &aSheet, true ) != searchRef )
                continue;

            for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
            {
                if( pin->GetName().CmpNoCase( aPinNumber ) == 0 )
                    return pin;
            }
        }
    }

    return nullptr;
}


// ---------------------------------------------------------------------------
// Helper: draw a single wire segment into an existing commit (no Push).
// After adding the wire, breaks any existing wires that pass through
// the start/end points as midpoints, so pins at those positions remain
// at wire endpoints and are visible to the connection graph.
// ---------------------------------------------------------------------------
static void drawWireSegment( SCH_EDIT_FRAME* frame, SCH_COMMIT* commit,
                              const VECTOR2I& aStart, const VECTOR2I& aEnd )
{
    SCH_LINE* wire = new SCH_LINE( aStart, LAYER_WIRE );
    wire->SetEndPoint( aEnd );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( wire, screen );
    commit->Added( wire, screen );

    frame->GetCanvas()->GetView()->Update( wire );

    // Break any existing wires that pass through our endpoints as midpoints.
    // This ensures pins at those positions are always at wire endpoints.
    if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
    {
        lwbTool->BreakSegments( commit, aStart, screen );
        lwbTool->BreakSegments( commit, aEnd, screen );
    }
}


// ---------------------------------------------------------------------------
// Tool: connect_pins
//
// Connects two pins with wires, automatically routing an L-shaped path
// (horizontal then vertical, or vertical then horizontal) so that no
// diagonal wires are ever created.  The LLM never needs to guess
// coordinates — it just says "connect U1 pin 8 to R1 pin 1".
//
// Arguments (JSON):
//   from_ref, from_pin — source reference designator + pin number
//   to_ref,   to_pin   — destination reference designator + pin number
//   route              — (optional) "HV" (horizontal-first, default) or "VH"
//
// Returns (JSON):
//   { "status": "ok", "from": {"ref":"U1","pin":"8","x":120,"y":75},
//     "to": {"ref":"R1","pin":"1","x":180,"y":50}, "segments": 2 }
// ---------------------------------------------------------------------------
static wxString handleConnectPins( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString fromRef  = wxString::FromUTF8( aArgs.value( "from_ref", "" ).c_str() );
    wxString fromPin = wxString::FromUTF8( aArgs.value( "from_pin", "" ).c_str() );
    wxString toRef    = wxString::FromUTF8( aArgs.value( "to_ref", "" ).c_str() );
    wxString toPin   = wxString::FromUTF8( aArgs.value( "to_pin", "" ).c_str() );
    wxString route   = wxString::FromUTF8( aArgs.value( "route", "HV" ).c_str() );

    if( fromRef.IsEmpty() || fromPin.IsEmpty() || toRef.IsEmpty() || toPin.IsEmpty() )
        return R"({ "error": "Missing from_ref/from_pin or to_ref/to_pin" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_PIN* srcPin = findPinByRef( screen, currentSheet, fromRef, fromPin );
    SCH_PIN* dstPin = findPinByRef( screen, currentSheet, toRef, toPin );

    if( !srcPin )
        return wxString::Format( R"({ "error": "Pin not found: %s pin %s" })", fromRef, fromPin );

    if( !dstPin )
        return wxString::Format( R"({ "error": "Pin not found: %s pin %s" })", toRef, toPin );

    VECTOR2I start = srcPin->GetPosition();
    VECTOR2I end   = dstPin->GetPosition();

    // Use a single commit for all wire segments, junctions, and breaks.
    // This prevents CleanUp() from merging collinear wires between commits
    // and hiding pin positions as wire midpoints.
    SCH_COMMIT commit( frame->GetToolManager() );

    int segments = 1;

    if( start == end )
    {
        // Pins are at the same location — nothing to draw
        segments = 0;
    }
    else if( start.x == end.x || start.y == end.y )
    {
        // Already aligned — single straight wire
        drawWireSegment( frame, &commit, start, end );
    }
    else
    {
        // Need an L-shaped path.  Pick the corner based on route preference.
        VECTOR2I corner;

        if( route == "VH" )
            corner = VECTOR2I( start.x, end.y ); // vertical first, then horizontal
        else
            corner = VECTOR2I( end.x, start.y ); // horizontal first, then vertical (default HV)

        drawWireSegment( frame, &commit, start, corner );
        drawWireSegment( frame, &commit, corner, end );

        // Add a junction at the corner so KiCad recognizes the two segments
        // as electrically connected, and break any wires passing through.
        SCH_JUNCTION* junction = new SCH_JUNCTION( corner );
        frame->AddToScreen( junction, screen );
        commit.Added( junction, screen );
        frame->GetCanvas()->GetView()->Update( junction );

        if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
            lwbTool->BreakSegments( &commit, corner, screen );

        segments = 2;
    }

    commit.Push( _( "Connect Pins (AI)" ) );

    // Recalculate the connection graph so ERC sees the new wires immediately.
    // Use LOCAL_CLEANUP (not GLOBAL_CLEANUP) because:
    //   1. commit.Push() above already triggered a LOCAL_CLEANUP
    //   2. run_erc will do its own GLOBAL_CLEANUP before running ERC
    //   3. GLOBAL_CLEANUP here runs CleanUp() on ALL screens, which can merge
    //      collinear wires and remove junctions that connect_pins just created,
    //      causing "Pin not connected" false positives
    if( SCHEMATIC* sch = &frame->Schematic() )
    {
        SCH_COMMIT recalcCommit( frame->GetToolManager() );
        sch->RecalculateConnections( &recalcCommit, LOCAL_CLEANUP, frame->GetToolManager() );
        recalcCommit.Push( _( "Recalculate (AI Connect)" ) );
    }

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["from"]["ref"] = std::string( fromRef.ToUTF8() );
    result["from"]["pin"] = std::string( fromPin.ToUTF8() );
    result["from"]["x"] = schIUScale.IUTomm( start.x );
    result["from"]["y"] = schIUScale.IUTomm( start.y );
    result["to"]["ref"] = std::string( toRef.ToUTF8() );
    result["to"]["pin"] = std::string( toPin.ToUTF8() );
    result["to"]["x"] = schIUScale.IUTomm( end.x );
    result["to"]["y"] = schIUScale.IUTomm( end.y );
    result["segments"] = segments;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: connect_label
//
// Places a net label at the exact position of a pin. This avoids the
// coordinate-rounding problem where the LLM places labels at integer
// coordinates that don't snap to the wire endpoint.
//
// Arguments (JSON):
//   ref         — reference designator, e.g. "U1"
//   pin         — pin number, e.g. "8"
//   text        — the net name, e.g. "VCC", "GND", "+5V"
//   label_type  — (optional) "local" (default), "global", or "hierarchical"
//
// Returns (JSON):
//   { "status": "ok", "text": "VCC", "ref": "U1", "pin": "8",
//     "position": { "x": 122.5, "y": 69.8 } }
// ---------------------------------------------------------------------------
static wxString handleConnectLabel( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString ref   = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    wxString pinNum = wxString::FromUTF8( aArgs.value( "pin", "" ).c_str() );
    wxString text  = wxString::FromUTF8( aArgs.value( "text", "" ).c_str() );
    wxString labelType = wxString::FromUTF8( aArgs.value( "label_type", "local" ).c_str() );

    if( ref.IsEmpty() || pinNum.IsEmpty() || text.IsEmpty() )
        return R"({ "error": "Missing 'ref', 'pin', or 'text' argument" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_PIN* pin = findPinByRef( screen, currentSheet, ref, pinNum );

    if( !pin )
        return wxString::Format( R"({ "error": "Pin not found: %s pin %s" })", ref, pinNum );

    VECTOR2I pinPos = pin->GetPosition();

    SCH_COMMIT commit( frame->GetToolManager() );

    if( labelType == "global" )
    {
        SCH_GLOBALLABEL* glabel = new SCH_GLOBALLABEL( pinPos, text );
        frame->AddToScreen( glabel, screen );
        commit.Added( glabel, screen );
        frame->GetCanvas()->GetView()->Update( glabel );
    }
    else
    {
        SCH_LABEL* label = new SCH_LABEL( pinPos, text );
        frame->AddToScreen( label, screen );
        commit.Added( label, screen );
        frame->GetCanvas()->GetView()->Update( label );
    }

    // Break any existing wires that pass through the label position as midpoints,
    // so the label connects to a wire endpoint, not a wire midpoint.
    if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
        lwbTool->BreakSegments( &commit, pinPos, screen );

    commit.Push( _( "Add Label (AI)" ) );

    // Recalculate the connection graph so the label is recognized immediately.
    // Use LOCAL_CLEANUP (not GLOBAL_CLEANUP) — same reasoning as connect_pins:
    // GLOBAL_CLEANUP runs CleanUp() on ALL screens, which can merge collinear
    // wires and remove junctions, causing "Pin not connected" false positives.
    if( SCHEMATIC* sch = &frame->Schematic() )
    {
        SCH_COMMIT recalcCommit( frame->GetToolManager() );
        sch->RecalculateConnections( &recalcCommit, LOCAL_CLEANUP, frame->GetToolManager() );
        recalcCommit.Push( _( "Recalculate (AI Label)" ) );
    }

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["text"] = std::string( text.ToUTF8() );
    result["ref"] = std::string( ref.ToUTF8() );
    result["pin"] = std::string( pinNum.ToUTF8() );
    result["position"]["x"] = schIUScale.IUTomm( pinPos.x );
    result["position"]["y"] = schIUScale.IUTomm( pinPos.y );

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
// Draws a wire between two points on the schematic.  If the two points are
// not horizontally or vertically aligned, the wire is automatically split
// into an L-shaped path (two orthogonal segments) so that no diagonal wires
// are ever created.  Prefer connect_pins when connecting two known pins.
//
// Arguments (JSON):
//   x1, y1 — start point in millimetres
//   x2, y2 — end point in millimetres
//   route  — (optional) "HV" (horizontal-first, default) or "VH"
//
// Returns (JSON):
//   { "status": "ok", "from": { "x": 25, "y": 25 }, "to": { "x": 50, "y": 25 },
//     "segments": 1, "diagonal_split": false }
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
    wxString route = wxString::FromUTF8( aArgs.value( "route", "HV" ).c_str() );

    VECTOR2I start = mmToGrid( x1, y1 );
    VECTOR2I end = mmToGrid( x2, y2 );

    SCH_SCREEN* screen = frame->GetScreen();
    int segments = 1;
    bool diagonalSplit = false;

    // Single commit for all segments + breaks
    SCH_COMMIT commit( frame->GetToolManager() );

    if( start.x == end.x || start.y == end.y )
    {
        // Already orthogonal — single straight wire
        drawWireSegment( frame, &commit, start, end );
    }
    else
    {
        // Diagonal requested — auto-split into L-shaped path
        diagonalSplit = true;
        segments = 2;

        VECTOR2I corner;
        if( route == "VH" )
            corner = VECTOR2I( start.x, end.y );
        else
            corner = VECTOR2I( end.x, start.y );

        drawWireSegment( frame, &commit, start, corner );
        drawWireSegment( frame, &commit, corner, end );

        // Add a junction at the corner and break any wires passing through
        SCH_JUNCTION* junction = new SCH_JUNCTION( corner );
        frame->AddToScreen( junction, screen );
        commit.Added( junction, screen );
        frame->GetCanvas()->GetView()->Update( junction );

        if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
            lwbTool->BreakSegments( &commit, corner, screen );
    }

    commit.Push( _( "Draw Wire (AI)" ) );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["from"]["x"] = x1;
    result["from"]["y"] = y1;
    result["to"]["x"] = x2;
    result["to"]["y"] = y2;
    result["segments"] = segments;
    result["diagonal_split"] = diagonalSplit;

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

    VECTOR2I pos = mmToGrid( xMm, yMm );

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

    VECTOR2I pos = mmToGrid( xMm, yMm );

    SCH_JUNCTION* junction = new SCH_JUNCTION( pos );

    SCH_SCREEN* screen = frame->GetScreen();
    frame->AddToScreen( junction, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( junction, screen );

    // Break any wires that pass through the junction position as midpoints
    if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
        lwbTool->BreakSegments( &commit, pos, screen );

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
// Adds a no-connect flag on a pin, marking it as intentionally unconnected.
//
// Arguments (JSON):
//   ref, pin — reference designator + pin number (preferred, looks up exact position)
//   x, y     — position in millimetres (fallback if ref/pin not given)
//
// Returns (JSON):
//   { "status": "ok", "position": { "x": 50, "y": 50 }, "ref": "U1", "pin": "5" }
// ---------------------------------------------------------------------------
static wxString handleAddNoConnect( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_SCREEN* screen = frame->GetScreen();
    VECTOR2I pos;
    bool usedRef = false;
    wxString refStr, pinStr;

    refStr = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    pinStr = wxString::FromUTF8( aArgs.value( "pin", "" ).c_str() );

    if( !refStr.IsEmpty() && !pinStr.IsEmpty() )
    {
        // Look up the pin by ref+pin number — exact position, no guessing
        SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();
        SCH_PIN* pin = findPinByRef( screen, currentSheet, refStr, pinStr );

        if( !pin )
            return wxString::Format( R"({ "error": "Pin not found: %s pin %s" })", refStr, pinStr );

        pos = pin->GetPosition();
        usedRef = true;

        // Check if this pin already has wires connected — placing a no_connect
        // on a wired pin causes "A pin with a no connection flag is connected"
        // ERC warning.  Reject the call so the LLM doesn't create this error.
        bool hasWires = false;
        for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
        {
            if( item->GetLayer() != LAYER_WIRE )
                continue;

            SCH_LINE* line = static_cast<SCH_LINE*>( item );

            if( line->IsEndPoint( pos ) )
            {
                hasWires = true;
                break;
            }
        }

        if( hasWires )
        {
            return wxString::Format( R"({ "error": "Pin %s.%s already has wires connected — cannot add no_connect. Remove the wire first or skip this pin." })",
                                     refStr, pinStr );
        }

        // Also check if a no_connect already exists at this position
        for( SCH_ITEM* item : screen->Items().OfType( SCH_NO_CONNECT_T ) )
        {
            if( item->GetPosition() == pos )
                return wxString::Format( R"({ "error": "No-connect already exists at %s pin %s" })", refStr, pinStr );
        }
    }
    else
    {
        // Fallback: use raw x/y coordinates
        double xMm = aArgs.value( "x", 0.0 );
        double yMm = aArgs.value( "y", 0.0 );
        pos = mmToGrid( xMm, yMm );
    }

    SCH_NO_CONNECT* nc = new SCH_NO_CONNECT( pos );

    frame->AddToScreen( nc, screen );

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Added( nc, screen );
    commit.Push( _( "Add No-Connect (AI)" ) );

    frame->GetCanvas()->GetView()->Update( nc );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["position"]["x"] = schIUScale.IUTomm( pos.x );
    result["position"]["y"] = schIUScale.IUTomm( pos.y );

    if( usedRef )
    {
        result["ref"] = std::string( refStr.ToUTF8() );
        result["pin"] = std::string( pinStr.ToUTF8() );
    }

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

    // Break all wires at all pin positions before running GLOBAL_CLEANUP.
    // CleanUp() merges collinear touching wires, which can turn a pin position
    // from a wire endpoint into a wire midpoint.  When that happens, the
    // connection graph no longer sees the pin as connected, and ERC reports
    // "Pin not connected" even though a wire passes through the pin position.
    // Breaking wires at pin positions ensures pins are always at wire endpoints.
    if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
    {
        SCH_SCREEN* breakScreen = screens.GetFirst();
        SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

        while( breakScreen )
        {
            SCH_COMMIT breakCommit( frame->GetToolManager() );

            for( SCH_ITEM* item : breakScreen->Items().OfType( SCH_SYMBOL_T ) )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                for( SCH_PIN* pin : symbol->GetPins( &currentSheet ) )
                {
                    VECTOR2I pinPos = pin->GetPosition();
                    lwbTool->BreakSegments( &breakCommit, pinPos, breakScreen );
                }
            }

            if( !breakCommit.Empty() )
                breakCommit.Push( _( "Break wires at pins (AI ERC)" ) );

            breakScreen = screens.GetNext();
        }
    }

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

            // Find which component and pin the marker is at.
            // This lets the LLM know exactly which pin to fix without
            // guessing from coordinates.
            SCH_SHEET_PATH& markerSheet = frame->GetCurrentSheet();
            for( SCH_ITEM* symItem : screen->Items().OfType( SCH_SYMBOL_T ) )
            {
                SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( symItem );
                wxString symRef = sym->GetRef( &markerSheet, true );

                for( SCH_PIN* pin : sym->GetPins( &markerSheet ) )
                {
                    if( pin->GetPosition() == pos )
                    {
                        entry["ref"] = std::string( symRef.ToUTF8() );
                        entry["pin"] = std::string( pin->GetNumber().ToUTF8() );
                        entry["pin_name"] = std::string( pin->GetName().ToUTF8() );
                        break;
                    }
                }

                if( entry.contains( "ref" ) )
                    break;
            }

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

    VECTOR2I newPos = mmToGrid( xMm, yMm );
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

    VECTOR2I pos = mmToGrid( xMm, yMm );

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
    else if( toolName == "clear_schematic" )
    {
        result = handleClearSchematic( aPanel, args );
    }
    else if( toolName == "get_pin_positions" )
    {
        result = handleGetPinPositions( aPanel, args );
    }
    else if( toolName == "connect_pins" )
    {
        result = handleConnectPins( aPanel, args );
    }
    else if( toolName == "connect_label" )
    {
        result = handleConnectLabel( aPanel, args );
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

        // Include pin positions so the LLM knows where to draw wires
        json pinsArray = json::array();
        std::vector<SCH_PIN*> pins = symbol->GetPins( &currentSheet );
        bool isMulti = symbol->IsMultiUnit();

        for( SCH_PIN* pin : pins )
        {
            // SCH_PIN::GetPosition() already returns the absolute schematic
            // position (transform applied + symbol offset added).  Using
            // GetPinPhysicalPosition() here would double-apply both, producing
            // coordinates at ~2x the correct location.
            VECTOR2I pinPos = pin->GetPosition();

            json pinEntry;
            pinEntry["name"] = std::string( pin->GetName().ToUTF8() );
            pinEntry["number"] = std::string( pin->GetNumber().ToUTF8() );
            pinEntry["x"] = schIUScale.IUTomm( pinPos.x );
            pinEntry["y"] = schIUScale.IUTomm( pinPos.y );

            // For multi-unit symbols (e.g. dual op-amps), include the
            // sub-unit ref so the LLM knows which sub-unit to address.
            if( isMulti )
            {
                int unit = pin->GetUnit();
                if( unit > 0 && unit <= 26 )
                {
                    wxString subRef = ref + wxChar( 'A' + unit - 1 );
                    pinEntry["sub_ref"] = std::string( subRef.ToUTF8() );
                }
            }

            // Include electrical type so the LLM can identify power pins,
            // passive pins (like Shield), and pins that need no_connects
            wxString elecType = pin->GetElectricalTypeName();
            pinEntry["electric_type"] = std::string( elecType.ToUTF8() );

            pinsArray.push_back( pinEntry );
        }

        entry["pins"] = pinsArray;

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

// ---------------------------------------------------------------------------
// Tool: clear_schematic
//
// Removes ALL items from the schematic — symbols, wires, labels, junctions,
// no-connects, and text. This is the "start fresh" tool.
//
// Arguments (JSON):
//   (none)
//
// Returns (JSON):
//   { "status": "ok", "removed": { "symbols": N, "wires": N, "labels": N, ... } }
// ---------------------------------------------------------------------------
static wxString handleClearSchematic( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_SCREEN* screen = frame->GetScreen();

    int symbolCount = 0;
    int wireCount = 0;
    int labelCount = 0;
    int junctionCount = 0;
    int noConnectCount = 0;
    int textCount = 0;

    SCH_COMMIT commit( frame->GetToolManager() );

    // Collect all items first, then remove
    std::vector<SCH_ITEM*> itemsToRemove;

    for( SCH_ITEM* item : screen->Items() )
    {
        switch( item->Type() )
        {
            case SCH_SYMBOL_T:
                symbolCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_LINE_T:
                wireCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_LABEL_T:
            case SCH_GLOBAL_LABEL_T:
            case SCH_HIER_LABEL_T:
                labelCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_JUNCTION_T:
                junctionCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_NO_CONNECT_T:
                noConnectCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_TEXT_T:
                textCount++;
                itemsToRemove.push_back( item );
                break;
            case SCH_BUS_WIRE_ENTRY_T:
            case SCH_BUS_BUS_ENTRY_T:
                // Bus entries must be removed too — otherwise they survive
                // clear_schematic and cause "Unconnected wire to bus entry"
                // ERC errors in every subsequent test run.
                itemsToRemove.push_back( item );
                break;
            default:
                break;
        }
    }

    // Remove all collected items
    for( SCH_ITEM* item : itemsToRemove )
    {
        frame->RemoveFromScreen( item, screen );
        commit.Removed( item, screen );
        frame->GetCanvas()->GetView()->Remove( item );
    }

    commit.Push( _( "Clear Schematic (AI)" ) );

    // Clear ERC markers
    SCH_SCREENS screens( frame->Schematic().Root() );
    screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, true );

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["removed"]["symbols"] = symbolCount;
    result["removed"]["wires"] = wireCount;
    result["removed"]["labels"] = labelCount;
    result["removed"]["junctions"] = junctionCount;
    result["removed"]["no_connects"] = noConnectCount;
    result["removed"]["texts"] = textCount;

    return wxString::FromUTF8( result.dump().c_str() );
}
