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
#include <wx/mstream.h>
#include <wx/image.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/dir.h>
#include <wx/filename.h>
#include <common.h>
#include <map>
#include <set>

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
#include <tool/actions.h>
#include <gal/graphics_abstraction_layer.h>
#include <view/view.h>
#include <base_units.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <reporter.h>
#include <symbol.h>

#include <project.h>
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <libraries/library_manager.h>
#include <libraries/library_table.h>
#include <pgm_base.h>
#include <erc/erc.h>
#include <sch_connection.h>
#include <connection_graph.h>
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
static wxString handleConnectNet( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleRedoLast( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleRemoveNoConnect( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleFindPart( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleCheck( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleUndoLast( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleAddNoConnects( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );

// Forward declaration — defined later, but needed by handleAddNoConnects
static SCH_PIN* findPinByRef( SCH_SCREEN* aScreen, SCH_SHEET_PATH& aSheet,
                                const wxString& aRef, const wxString& aPinNumber );

static wxString handleScreenshot( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );
static wxString handleGetSymbolInfo( AI_ASSISTANT_PANEL* aPanel, const json& aArgs );


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


// A net label or a no-connect marks a PIN, but KiCad stores it as a coordinate
// and nothing ties the two together. So when a symbol moves or is deleted, the
// marker stays exactly where it was.
//
// That is not cosmetic. The pin it used to mark silently leaves its net, and the
// marker becomes an orphan sitting over empty space, which ERC reports as
// "Label not connected". A sheet can end up carrying twice as many labels as
// pins, half of them meaningless, while every tool involved reported success --
// which is precisely what happened: three parts, six pins, twelve labels.
//
// So markers are collected together WITH the pin they belong to before the
// symbol is touched, and afterwards either moved to wherever that pin ended up
// or removed along with it.
struct PIN_MARKER
{
    SCH_ITEM* item;
    SCH_PIN*  pin;
};


// Is there anything at this point for a label to attach to?
//
// A label is only meaningful where a pin or a wire is. Sitting anywhere else it
// names nothing, contributes nothing to the netlist, and shows up in ERC as
// "Label not connected" -- a warning that reads like a wiring mistake but is
// really just litter.
static bool hasConnectableAt( SCH_SCREEN* aScreen, SCH_SHEET_PATH& aSheet, const VECTOR2I& aPos )
{
    for( SCH_ITEM* item : aScreen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
        {
            if( pin->GetPosition() == aPos )
                return true;
        }
    }

    // A wire counts anywhere along it, not only at its ends: KiCad breaks a
    // segment at a label placed mid-run.
    for( SCH_ITEM* item : aScreen->Items().OfType( SCH_LINE_T ) )
    {
        SCH_LINE* line = static_cast<SCH_LINE*>( item );

        if( line->GetLayer() != LAYER_WIRE )
            continue;

        if( line->GetStartPoint() == aPos || line->GetEndPoint() == aPos
            || line->HitTest( aPos, 0 ) )
            return true;
    }

    return false;
}


static std::vector<PIN_MARKER> collectPinMarkers( SCH_SCREEN* aScreen, SCH_SYMBOL* aSymbol,
                                                  SCH_SHEET_PATH& aSheet )
{
    std::vector<PIN_MARKER> found;

    const KICAD_T markerTypes[] = { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T,
                                    SCH_NO_CONNECT_T };

    for( SCH_PIN* pin : aSymbol->GetPins( &aSheet ) )
    {
        VECTOR2I pinPos = pin->GetPosition();

        for( KICAD_T type : markerTypes )
        {
            for( SCH_ITEM* item : aScreen->Items().OfType( type ) )
            {
                if( item->GetPosition() == pinPos )
                    found.push_back( { item, pin } );
            }
        }
    }

    return found;
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

    // Assign a concrete reference if the caller did not give one.
    //
    // Without this KiCad uses the symbol's default, which is the prefix plus a
    // question mark -- "R?" -- and the sheet then needs annotate() to turn every
    // "?" into a number. Annotation RENUMBERS, so a part placed as "D" could come
    // back as "D101", and anything holding the old reference was then pointing at
    // something that no longer existed.
    //
    // Picking the next free number here makes the reference final the moment the
    // part lands, and it is returned to the caller immediately.
    if( fieldReference.IsEmpty() )
    {
        // Prefix comes from the symbol itself (R for resistors, C, U, D...).
        wxString prefix = libSymbol->GetReferenceField().GetText();
        prefix.Replace( wxS( "?" ), wxEmptyString );

        if( prefix.IsEmpty() )
            prefix = wxS( "U" );

        // Highest number already used by that prefix on this sheet.
        int highest = 0;

        for( SCH_ITEM* item : frame->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            wxString existing = static_cast<SCH_SYMBOL*>( item )->GetRef( &currentSheet, true );

            if( !existing.StartsWith( prefix ) )
                continue;

            wxString digits = existing.Mid( prefix.Length() );
            long     n = 0;

            if( digits.ToLong( &n ) && n > highest )
                highest = (int) n;
        }

        fieldReference = wxString::Format( wxS( "%s%d" ), prefix, highest + 1 );
    }

    picked.Fields.emplace_back( FIELD_T::REFERENCE, fieldReference );

    // Position. If the caller did not give one, find a free spot rather than
    // making the caller invent coordinates.
    //
    // x and y used to be required, and a language model has no spatial sense --
    // it produced overlapping parts, and coordinates outside the page that were
    // then clamped so several components collapsed onto the same corner. It is
    // the same failure as inventing footprints: a required field with no real
    // source. Layout is arithmetic, so the code should do it.
    const bool havePosition = aArgs.contains( "x" ) && aArgs.contains( "y" );

    double xMm = aArgs.value( "x", 0.0 );
    double yMm = aArgs.value( "y", 0.0 );

    if( !havePosition )
    {
        // Where everything already sits.
        std::vector<VECTOR2I> taken;

        for( SCH_ITEM* item : frame->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
            taken.push_back( item->GetPosition() );

        // Walk the printable area left to right, top to bottom, and stop at the
        // first slot far enough from everything already placed. The step is wide
        // enough that symbol bodies and their reference/value text do not collide.
        const double STEP_X = 30.0;
        const double STEP_Y = 35.0;
        const double CLEARANCE_MM = 20.0;

        bool found = false;

        for( double y = 40.0; y <= 180.0 && !found; y += STEP_Y )
        {
            for( double x = 40.0; x <= 260.0 && !found; x += STEP_X )
            {
                VECTOR2I candidate = mmToGrid( x, y );
                bool     clear = true;

                for( const VECTOR2I& other : taken )
                {
                    double dx = schIUScale.IUTomm( std::abs( candidate.x - other.x ) );
                    double dy = schIUScale.IUTomm( std::abs( candidate.y - other.y ) );

                    if( dx < CLEARANCE_MM && dy < CLEARANCE_MM )
                    {
                        clear = false;
                        break;
                    }
                }

                if( clear )
                {
                    xMm = x;
                    yMm = y;
                    found = true;
                }
            }
        }

        // Sheet is full. Place it anyway rather than refusing — the caller can
        // move things, and check() reports the overlap.
        if( !found )
        {
            xMm = 40.0;
            yMm = 40.0;
        }
    }

    // Clamp to the printable page area (A4 = 297x210mm, with margins)
    // Components placed outside this area are invisible in print/exports
    // and look broken to the user.
    //
    // The clamp is reported back in the result. Clamping silently used to make
    // several out-of-bounds components collapse onto the same corner, which then
    // looked like a placement bug rather than a plan bug.
    const double requestedX = xMm;
    const double requestedY = yMm;

    if( xMm < 20.0 ) xMm = 20.0;
    if( xMm > 270.0 ) xMm = 270.0;
    if( yMm < 20.0 ) yMm = 20.0;
    if( yMm > 190.0 ) yMm = 190.0;

    const bool wasClamped = ( xMm != requestedX ) || ( yMm != requestedY );

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

    if( wasClamped )
    {
        result["clamped"] = true;
        result["requested_position"]["x"] = requestedX;
        result["requested_position"]["y"] = requestedY;
        result["warning"] = "Requested position was outside the printable page area "
                            "(x 20-270mm, y 20-190mm) and was clamped. Choose "
                            "coordinates inside that range so components do not stack.";
    }

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: add_no_connects (batch)
//
// Adds no-connect flags on multiple pins in a single tool call.
// This is critical for high-pin-count ICs (ESP32, ATmega328, etc.) where
// calling add_no_connect 30+ times would exceed the tool call limit.
//
// Arguments (JSON):
//   pins — array of { "ref": "U1", "pin": "3" } objects
//
// Returns (JSON):
//   { "status": "ok", "added": 28, "skipped": 2, "errors": [...] }
// ---------------------------------------------------------------------------
static wxString handleAddNoConnects( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    if( !aArgs.contains( "pins" ) || !aArgs["pins"].is_array() )
        return R"({ "error": "Missing 'pins' array argument" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_COMMIT commit( frame->GetToolManager() );

    int added = 0;
    int skipped = 0;
    json errors = json::array();

    for( const auto& pinSpec : aArgs["pins"] )
    {
        wxString refStr = wxString::FromUTF8( pinSpec.value( "ref", "" ).c_str() );
        wxString pinStr = wxString::FromUTF8( pinSpec.value( "pin", "" ).c_str() );

        if( refStr.IsEmpty() || pinStr.IsEmpty() )
        {
            skipped++;
            continue;
        }

        SCH_PIN* pin = findPinByRef( screen, currentSheet, refStr, pinStr );

        if( !pin )
        {
            json err = { { "ref", std::string( refStr.ToUTF8() ) },
                         { "pin", std::string( pinStr.ToUTF8() ) },
                         { "error", "Pin not found" } };
            errors.push_back( err );
            skipped++;
            continue;
        }

        VECTOR2I pos = pin->GetPosition();

        // Check if pin already has wires
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
            json err = { { "ref", std::string( refStr.ToUTF8() ) },
                         { "pin", std::string( pinStr.ToUTF8() ) },
                         { "error", "Pin already has wires" } };
            errors.push_back( err );
            skipped++;
            continue;
        }

        // Check if no_connect already exists
        bool exists = false;
        for( SCH_ITEM* item : screen->Items().OfType( SCH_NO_CONNECT_T ) )
        {
            if( item->GetPosition() == pos )
            {
                exists = true;
                break;
            }
        }

        if( exists )
        {
            skipped++;
            continue;
        }

        SCH_NO_CONNECT* nc = new SCH_NO_CONNECT( pos );
        frame->AddToScreen( nc, screen );
        commit.Added( nc, screen );
        frame->GetCanvas()->GetView()->Update( nc );
        added++;
    }

    if( added > 0 )
        commit.Push( _( "Add No-Connects (AI)" ) );

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["added"] = added;
    result["skipped"] = skipped;
    if( !errors.empty() )
        result["errors"] = errors;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: undo_last
//
// Undoes the last action (or last N actions) performed by the AI agent.
// Uses KiCad's built-in undo stack — each tool call that modifies the
// schematic pushes an undo entry, so this just pops it.
//
// Arguments (JSON):
//   steps — (optional) number of actions to undo, default 1
//
// Returns (JSON):
//   { "status": "ok", "undone": 2 }
//   or { "error": "Nothing to undo" }
// ---------------------------------------------------------------------------
static wxString handleUndoLast( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    int steps = aArgs.value( "steps", 1 );
    if( steps < 1 )
        steps = 1;
    if( steps > 20 )
        steps = 20;

    int undone = 0;

    for( int i = 0; i < steps; i++ )
    {
        // Check if there's anything to undo
        if( !frame->GetScreen() || frame->GetUndoCommandCount() == 0 )
            break;

        frame->GetToolManager()->RunAction( ACTIONS::undo );
        undone++;
    }

    if( undone == 0 )
        return R"({ "error": "Nothing to undo — the undo stack is empty" })";

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["undone"] = undone;
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

    // Only push if wires were actually created. Pushing an empty commit adds an
    // undo entry that undoes nothing, which throws off undo_last(steps=N).
    if( !commit.Empty() )
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

        // Only push if the recalculation actually changed something. Pushed
        // unconditionally, this became a SECOND undo entry for one connect_pins
        // call — so undo_last(steps=1) undid the recalculation and left the wires
        // in place, while the caller believed the connection had been removed.
        if( !recalcCommit.Empty() )
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

        // See connect_pins: an unconditional push here made one connect_label
        // cost two undo steps.
        if( !recalcCommit.Empty() )
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
// Tool: connect_net
//
// Puts every listed pin on the same net by placing a net label with the same
// name at each pin's exact position, in ONE call and ONE undo step.
//
// This is the preferred way to express connectivity. Wiring pins pairwise with
// connect_pins expresses a 3-pin net as two overlapping L-routes, and KiCad's
// CleanUp() then merges collinear wires -- which turns a pin position from a
// wire ENDPOINT into a wire MIDPOINT, at which point the connection graph stops
// seeing that pin as connected and ERC reports "Pin not connected" for a pin
// that visually has a wire on it.
//
// A label has no geometry to get wrong: it attaches at the pin position, and
// pins sharing a net name are connected no matter where they sit on the sheet.
//
// Arguments (JSON):
//   name — the net name, e.g. "+5V", "GND", "SDA"
//   pins — array of either { "ref": "U1", "pin": "8" } objects or "U1.8" strings
//   label_type — (optional) "local" (default) or "global"
//
// Returns (JSON):
//   { "status": "ok", "net": "+5V", "connected": 3, "skipped": 0,
//     "pins": ["R1.1", "U1.8", "C1.1"], "errors": [...] }
// ---------------------------------------------------------------------------
static wxString handleConnectNet( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString netName = wxString::FromUTF8( aArgs.value( "name", "" ).c_str() );

    if( netName.IsEmpty() )
        return R"({ "error": "Missing 'name' argument — the net name, e.g. \"GND\"" })";

    if( !aArgs.contains( "pins" ) || !aArgs["pins"].is_array() )
        return R"({ "error": "Missing 'pins' array argument, e.g. [{\"ref\":\"R1\",\"pin\":\"1\"}]" })";

    // A single-pin net is allowed. Putting one pin on "+5V" is how a supply rail
    // is expressed, and step-by-step building creates one-pin nets routinely --
    // the second pin arrives on a later turn. Rejecting them forced the caller to
    // invent two-pin nets with meaningless names just to get past the check.
    if( aArgs["pins"].empty() )
    {
        return R"({ "error": "The 'pins' array is empty. List at least one pin to put on this net." })";
    }

    wxString labelType = wxString::FromUTF8( aArgs.value( "label_type", "local" ).c_str() );

    SCH_SCREEN*     screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_COMMIT commit( frame->GetToolManager() );

    int  connected = 0;
    int  skipped = 0;
    json errors = json::array();
    json connectedPins = json::array();
    json clearedNoConnects = json::array();

    // Sweep away labels carrying THIS net's name that are attached to nothing.
    //
    // Scoped to the net being built on purpose: a stray label of some other name
    // may be work in progress on a net nobody has finished yet, and deleting it
    // would be a surprise. One carrying the name we are about to place is
    // unambiguously litter -- it names this net but contributes nothing to it.
    int sweptStrays = 0;

    {
        std::vector<SCH_ITEM*> strays;
        const KICAD_T labelTypes[] = { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T };

        for( KICAD_T type : labelTypes )
        {
            for( SCH_ITEM* item : screen->Items().OfType( type ) )
            {
                SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );

                if( label->GetText() == netName
                    && !hasConnectableAt( screen, currentSheet, label->GetPosition() ) )
                {
                    strays.push_back( item );
                }
            }
        }

        for( SCH_ITEM* stray : strays )
        {
            frame->RemoveFromScreen( stray, screen );
            commit.Removed( stray, screen );
            frame->GetCanvas()->GetView()->Remove( stray );
            sweptStrays++;
        }
    }

    for( const auto& pinSpec : aArgs["pins"] )
    {
        wxString refStr;
        wxString pinStr;

        // Accept both { "ref": "U1", "pin": "8" } and the terser "U1.8" form.
        if( pinSpec.is_string() )
        {
            wxString combined = wxString::FromUTF8( pinSpec.get<std::string>().c_str() );
            refStr = combined.BeforeLast( '.' );
            pinStr = combined.AfterLast( '.' );
        }
        else if( pinSpec.is_object() )
        {
            refStr = wxString::FromUTF8( pinSpec.value( "ref", "" ).c_str() );
            pinStr = wxString::FromUTF8( pinSpec.value( "pin", "" ).c_str() );
        }

        if( refStr.IsEmpty() || pinStr.IsEmpty() )
        {
            errors.push_back( { { "pin", pinSpec.dump() },
                                { "error", "Could not read ref and pin from this entry" } } );
            skipped++;
            continue;
        }

        // findPinByRef matches on pin number first and pin name second, so
        // callers may pass either "8" or "VCC" without translating.
        SCH_PIN* pin = findPinByRef( screen, currentSheet, refStr, pinStr );

        if( !pin )
        {
            errors.push_back( { { "ref", std::string( refStr.ToUTF8() ) },
                                { "pin", std::string( pinStr.ToUTF8() ) },
                                { "error", "Pin not found on the schematic" } } );
            skipped++;
            continue;
        }

        VECTOR2I pinPos = pin->GetPosition();

        // A no-connect and a net label on the same pin contradict each other.
        //
        // Naming a pin in a net is an explicit statement that it carries a signal,
        // so it WINS: the stale no-connect is removed rather than the call being
        // refused. Refusing produced a dead end — the error told the caller to
        // "remove the no_connect first", but no tool can do that, so the same call
        // was retried until the round ran out.
        //
        // The reverse direction stays blocked, in add_no_connects: putting a
        // no-connect on a pin that already has a wire is simply wrong, not a
        // change of intent.
        SCH_NO_CONNECT* staleNoConnect = nullptr;

        for( SCH_ITEM* item : screen->Items().OfType( SCH_NO_CONNECT_T ) )
        {
            if( item->GetPosition() == pinPos )
            {
                staleNoConnect = static_cast<SCH_NO_CONNECT*>( item );
                break;
            }
        }

        if( staleNoConnect )
        {
            commit.Removed( staleNoConnect, screen );
            frame->RemoveFromScreen( staleNoConnect, screen );
            frame->GetCanvas()->GetView()->Remove( staleNoConnect );

            clearedNoConnects.push_back(
                    std::string( ( refStr + wxS( "." ) + pinStr ).ToUTF8() ) );
        }

        // Inspect any label already sitting on this pin.
        //
        // Same name  -> already done, skip quietly.
        // DIFFERENT name -> refuse. Two different net names at one point ties
        // those two nets together, which is a short. It is the most dangerous
        // failure mode here because the schematic still looks correct, so this
        // must be reported rather than silently added.
        bool     alreadyLabelled = false;
        bool     conflicting = false;
        wxString conflictingName;

        const KICAD_T checkTypes[] = { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T };

        for( KICAD_T checkType : checkTypes )
        {
            for( SCH_ITEM* item : screen->Items().OfType( checkType ) )
            {
                SCH_LABEL_BASE* existing = static_cast<SCH_LABEL_BASE*>( item );

                if( existing->GetPosition() != pinPos )
                    continue;

                if( existing->GetText() == netName )
                {
                    alreadyLabelled = true;
                }
                else
                {
                    conflicting = true;
                    conflictingName = existing->GetText();
                }

                break;
            }

            if( alreadyLabelled || conflicting )
                break;
        }

        if( conflicting )
        {
            errors.push_back( { { "ref", std::string( refStr.ToUTF8() ) },
                                { "pin", std::string( pinStr.ToUTF8() ) },
                                { "error", wxString::Format(
                                      "Pin already carries net \"%s\". Adding \"%s\" here would "
                                      "short the two nets together. Remove the existing label "
                                      "first, or leave this pin off this net.",
                                      conflictingName, netName ).ToStdString() } } );
            skipped++;
            continue;
        }

        if( alreadyLabelled )
        {
            connectedPins.push_back( std::string( ( refStr + wxS( "." ) + pinStr ).ToUTF8() ) );
            connected++;
            continue;
        }

        if( labelType == "global" )
        {
            SCH_GLOBALLABEL* glabel = new SCH_GLOBALLABEL( pinPos, netName );
            frame->AddToScreen( glabel, screen );
            commit.Added( glabel, screen );
            frame->GetCanvas()->GetView()->Update( glabel );
        }
        else
        {
            SCH_LABEL* label = new SCH_LABEL( pinPos, netName );
            frame->AddToScreen( label, screen );
            commit.Added( label, screen );
            frame->GetCanvas()->GetView()->Update( label );
        }

        // If a wire already runs through this pin as a midpoint, break it so the
        // pin sits at an endpoint and the connection graph sees it.
        if( SCH_LINE_WIRE_BUS_TOOL* lwbTool = frame->GetToolManager()->GetTool<SCH_LINE_WIRE_BUS_TOOL>() )
            lwbTool->BreakSegments( &commit, pinPos, screen );

        connectedPins.push_back( std::string( ( refStr + wxS( "." ) + pinStr ).ToUTF8() ) );
        connected++;
    }

    // One commit for the whole net, so undo_last(1) removes the entire net
    // rather than one label at a time.
    if( !commit.Empty() )
        commit.Push( _( "Connect Net (AI)" ) );

    if( SCHEMATIC* sch = &frame->Schematic() )
    {
        SCH_COMMIT recalcCommit( frame->GetToolManager() );
        sch->RecalculateConnections( &recalcCommit, LOCAL_CLEANUP, frame->GetToolManager() );

        if( !recalcCommit.Empty() )
            recalcCommit.Push( _( "Recalculate (AI Connect Net)" ) );
    }

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["net"] = std::string( netName.ToUTF8() );
    result["connected"] = connected;
    result["skipped"] = skipped;
    result["pins"] = connectedPins;

    if( !errors.empty() )
        result["errors"] = errors;

    if( sweptStrays > 0 )
    {
        result["removed_stray_labels"] = sweptStrays;
        result["stray_note"] = "Labels with this net's name were sitting on nothing and have "
                               "been removed. They were left behind by an earlier edit.";
    }

    if( !clearedNoConnects.empty() )
    {
        result["cleared_no_connects"] = clearedNoConnects;
        result["note"] = "These pins had a no-connect marker, which was removed so they "
                         "could join this net. If a pin really is unused, leave it out of "
                         "the net instead of no-connecting it afterwards.";
    }

    if( connected < 2 )
    {
        result["warning"] = "Fewer than 2 pins were connected, so this is not actually a net. "
                            "Check the errors and re-issue with pins that exist.";
    }

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

    // Reject an unknown property BEFORE opening a commit, so nothing is snapshot
    // for a change that is not going to happen.
    if( property != "value" && property != "footprint" && property != "reference" )
    {
        return wxString::Format( R"({ "error": "Unknown property: %s. Use 'value', 'footprint', or 'reference'." })", property );
    }

    // Snapshot BEFORE mutating, using Modify() not Modified() — see the note in
    // handleMoveSymbol. Modified(item, aCopy, screen) was silently taking the
    // SCREEN as the item's "before" copy and corrupting the undo stack.
    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Modify( target, screen );

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

    // Collect results with relevance score for sorting
    struct SearchResult
    {
        wxString library_name;
        wxString name;
        wxString library;
        wxString description;
        wxString default_value;
        int relevance; // lower = more relevant
    };

    // Cap at 20 results — the LLM can read 20 sorted results and pick the right
    // one, but it cannot read hundreds. See also: relevance ordering below.
    const int MAX_RESULTS = 20;

    // Description matching requires parsing the symbol off disk. The standard
    // KiCad libraries hold ~23,000 symbols in ~223 libraries, so loading every
    // symbol on every search parses ~23,000 files on the UI thread and freezes
    // the whole application for seconds at a time.
    //
    // Instead: match on symbol and library NAME first, which needs no disk
    // access at all, and only load the handful of symbols we are actually going
    // to return. Description matching stays available, but runs only when the
    // name passes find nothing, and is bounded so it can never freeze the UI.
    const int MAX_DESCRIPTION_SCAN = 4000;

    std::vector<SearchResult> results;

    for( const wxString& libNickname : libNames )
    {
        std::vector<wxString> symNames = adapter->GetSymbolNames( libNickname );

        wxString lowerLib = libNickname;
        lowerLib.LowerCase();

        for( const wxString& symName : symNames )
        {
            wxString fullId = libNickname + wxS( ":" ) + symName;

            if( query.IsEmpty() )
            {
                // No query — everything is equally (ir)relevant; capped below.
                results.push_back( { fullId, symName, libNickname, wxEmptyString,
                                     wxEmptyString, 5 } );
                continue;
            }

            wxString lowerSym = symName;
            lowerSym.LowerCase();

            // Relevance: 0 = exact name, 1 = name starts with, 2 = name contains,
            // 3 = library name contains. Description matches (4) are handled in
            // the bounded fallback pass below.
            int relevance = -1;

            if( lowerSym == query )
                relevance = 0;
            else if( lowerSym.StartsWith( query ) )
                relevance = 1;
            else if( lowerSym.Contains( query ) )
                relevance = 2;
            else if( lowerLib.Contains( query ) )
                relevance = 3;
            else
                continue; // no name-level match

            results.push_back( { fullId, symName, libNickname, wxEmptyString,
                                 wxEmptyString, relevance } );
        }
    }

    // Fallback: nothing matched by name, so the query is probably descriptive
    // ("current sense amplifier"). Scan descriptions, but stop as soon as we
    // have enough results or have examined MAX_DESCRIPTION_SCAN symbols.
    bool descriptionScanTruncated = false;

    if( results.empty() && !query.IsEmpty() )
    {
        int examined = 0;

        for( const wxString& libNickname : libNames )
        {
            if( (int) results.size() >= MAX_RESULTS || examined >= MAX_DESCRIPTION_SCAN )
                break;

            for( const wxString& symName : adapter->GetSymbolNames( libNickname ) )
            {
                if( (int) results.size() >= MAX_RESULTS || examined >= MAX_DESCRIPTION_SCAN )
                {
                    descriptionScanTruncated = true;
                    break;
                }

                examined++;

                LIB_SYMBOL* libSym = adapter->LoadSymbol( libNickname, symName );

                if( !libSym )
                    continue;

                wxString description = libSym->GetDescription();
                wxString lowerDesc = description;
                lowerDesc.LowerCase();

                if( !lowerDesc.Contains( query ) )
                    continue;

                results.push_back( { libNickname + wxS( ":" ) + symName, symName,
                                     libNickname, description,
                                     libSym->GetValueField().GetText(), 4 } );
            }
        }
    }

    // Stable sort so that within the same relevance band the library order from
    // the symbol table is preserved, which keeps results reproducible run to run.
    std::stable_sort( results.begin(), results.end(),
        []( const SearchResult& a, const SearchResult& b ) {
            return a.relevance < b.relevance;
        } );

    int count = std::min( (int) results.size(), MAX_RESULTS );

    json symbolsArray = json::array();

    for( int i = 0; i < count; i++ )
    {
        SearchResult& r = results[i];

        // Load the symbol only now, for the <= 20 entries we are returning, to
        // fill in description and default value.
        if( r.description.IsEmpty() && r.default_value.IsEmpty() )
        {
            if( LIB_SYMBOL* libSym = adapter->LoadSymbol( r.library, r.name ) )
            {
                r.description = libSym->GetDescription();
                r.default_value = libSym->GetValueField().GetText();
            }
        }

        json entry;
        entry["library_name"] = std::string( r.library_name.ToUTF8() );
        entry["name"] = std::string( r.name.ToUTF8() );
        entry["library"] = std::string( r.library.ToUTF8() );
        entry["description"] = std::string( r.description.ToUTF8() );
        entry["default_value"] = std::string( r.default_value.ToUTF8() );

        symbolsArray.push_back( entry );
    }

    json result;
    result["count"] = count;
    result["total_matches"] = (int) results.size();
    result["symbols"] = symbolsArray;

    if( descriptionScanTruncated )
    {
        result["note"] = "No symbol or library name matched, so descriptions were "
                         "searched and the scan was truncated. Try a shorter or more "
                         "specific keyword that appears in the symbol name.";
    }

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: search_footprints
//
// Searches KiCad's footprint library table for footprint libraries matching
// a keyword. Returns the list of available footprint libraries.
//
// Returns REAL footprint IDs, not just library names. KiCad footprint
// libraries are ".pretty" directories holding one ".kicad_mod" file per
// footprint, so the footprint names are enumerated by listing those files.
// This needs no pcbnew internals.
//
// Returning real IDs matters: the caller previously had to invent footprint
// names from "standard naming conventions", which produced footprints that do
// not exist and silently broke the resulting board.
//
// Arguments (JSON):
//   query   — (optional) keyword matched against the footprint name and the
//             library name, e.g. "R_Axial", "DIP-8", "LED_D3".
//             Supports a trailing "*" wildcard, so KiCad footprint filters
//             taken from get_symbol_info (e.g. "R_*") can be passed straight in.
//   library — (optional) restrict the search to one library nickname.
//
// Returns (JSON):
//   { "count": 20, "total_matches": 137,
//     "footprints": [ { "footprint": "Resistor_THT:R_Axial_DIN0207...",
//                       "name": "R_Axial_DIN0207...", "library": "Resistor_THT" } ],
//     "libraries": [ { "name": "Resistor_THT", "type": "footprint_library" } ] }
// ---------------------------------------------------------------------------
static wxString handleSearchFootprints( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString query = wxString::FromUTF8( aArgs.value( "query", "" ).c_str() );
    query.LowerCase();

    // KiCad footprint filters arrive as glob patterns like "R_*" or "LED_THT:*".
    // Strip the library prefix and the wildcard so a filter can be passed
    // through unmodified by the caller.
    if( query.Contains( wxS( ":" ) ) )
        query = query.AfterLast( ':' );

    query.Replace( wxS( "*" ), wxEmptyString );
    query.Replace( wxS( "?" ), wxEmptyString );
    query.Trim( true ).Trim( false );

    wxString libFilter = wxString::FromUTF8( aArgs.value( "library", "" ).c_str() );
    libFilter.LowerCase();

    LIBRARY_MANAGER& libMgr = Pgm().GetLibraryManager();
    std::vector<LIBRARY_TABLE_ROW*> rows = libMgr.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT );

    // Cap results for the same reason search_symbols does — a long unsorted list
    // is worse than a short relevant one.
    const int MAX_RESULTS = 20;

    struct FootprintResult
    {
        wxString library;
        wxString name;
        int      relevance; // lower = more relevant
    };

    std::vector<FootprintResult> matches;
    json libsArray = json::array();
    int  libCount = 0;

    for( const LIBRARY_TABLE_ROW* row : rows )
    {
        wxString libName = row->Nickname();
        wxString lowerLib = libName;
        lowerLib.LowerCase();

        if( !libFilter.IsEmpty() && lowerLib != libFilter )
            continue;

        json libEntry;
        libEntry["name"] = std::string( libName.ToUTF8() );
        libEntry["type"] = "footprint_library";
        libsArray.push_back( libEntry );
        libCount++;

        // Resolve ${KICAD10_FOOTPRINT_DIR} and friends to a real path.
        wxString uri = ExpandEnvVarSubstitutions( row->URI(), &frame->Prj() );

        if( uri.IsEmpty() || !wxDirExists( uri ) )
            continue;

        wxDir dir( uri );

        if( !dir.IsOpened() )
            continue;

        wxString fileName;
        bool     hasFile = dir.GetFirst( &fileName, wxS( "*.kicad_mod" ), wxDIR_FILES );

        while( hasFile )
        {
            wxString fpName = fileName.BeforeLast( '.' );
            wxString lowerFp = fpName;
            lowerFp.LowerCase();

            int relevance = -1;

            if( query.IsEmpty() )
                relevance = 5;
            else if( lowerFp == query )
                relevance = 0;
            else if( lowerFp.StartsWith( query ) )
                relevance = 1;
            else if( lowerFp.Contains( query ) )
                relevance = 2;
            else if( lowerLib.Contains( query ) )
                relevance = 3;

            if( relevance >= 0 )
                matches.push_back( { libName, fpName, relevance } );

            hasFile = dir.GetNext( &fileName );
        }
    }

    std::stable_sort( matches.begin(), matches.end(),
        []( const FootprintResult& a, const FootprintResult& b ) {
            return a.relevance < b.relevance;
        } );

    int count = std::min( (int) matches.size(), MAX_RESULTS );

    json footprintsArray = json::array();

    for( int i = 0; i < count; i++ )
    {
        const FootprintResult& m = matches[i];

        json entry;
        entry["footprint"] = std::string( ( m.library + wxS( ":" ) + m.name ).ToUTF8() );
        entry["name"] = std::string( m.name.ToUTF8() );
        entry["library"] = std::string( m.library.ToUTF8() );

        footprintsArray.push_back( entry );
    }

    json result;
    result["count"] = count;
    result["total_matches"] = (int) matches.size();
    result["footprints"] = footprintsArray;
    result["libraries"] = libsArray;
    result["library_count"] = libCount;
    result["note"] = "The 'footprint' field of each result is a real, verified footprint ID. "
                     "Use it verbatim. Do not construct footprint IDs by hand.";

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

            // Collect every wire endpoint once, up front. BreakSegments() is
            // only needed for a pin that sits in the MIDDLE of a wire — a pin
            // already at an endpoint is visible to the connection graph as-is.
            //
            // Calling BreakSegments() unconditionally for every pin made this
            // O(pins x wires) and was a major source of UI freezes on parts with
            // high pin counts. Guarding on the endpoint set skips the vast
            // majority of those calls while preserving the midpoint fix.
            std::set<std::pair<int, int>> wireEndpoints;

            for( SCH_ITEM* item : breakScreen->Items().OfType( SCH_LINE_T ) )
            {
                if( item->GetLayer() != LAYER_WIRE )
                    continue;

                SCH_LINE* line = static_cast<SCH_LINE*>( item );
                VECTOR2I  start = line->GetStartPoint();
                VECTOR2I  end = line->GetEndPoint();

                wireEndpoints.emplace( start.x, start.y );
                wireEndpoints.emplace( end.x, end.y );
            }

            for( SCH_ITEM* item : breakScreen->Items().OfType( SCH_SYMBOL_T ) )
            {
                SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

                for( SCH_PIN* pin : symbol->GetPins( &currentSheet ) )
                {
                    VECTOR2I pinPos = pin->GetPosition();

                    if( wireEndpoints.count( { pinPos.x, pinPos.y } ) )
                        continue; // already an endpoint — nothing to break

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

    // run_erc is a read as far as the caller is concerned. Pushing here
    // unconditionally added an undo entry every time ERC ran, so a few ERC passes
    // silently buried the caller's real edits under entries that undo nothing.
    if( !commit.Empty() )
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

    // Clamp to the printable page area (A4 = 297x210mm, with margins)
    if( xMm < 20.0 ) xMm = 20.0;
    if( xMm > 270.0 ) xMm = 270.0;
    if( yMm < 20.0 ) yMm = 20.0;
    if( yMm > 190.0 ) yMm = 190.0;

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

    // Snapshot BEFORE mutating, using Modify() rather than Modified().
    //
    // The old code mutated the symbol first and then called
    //     commit.Modified( target, screen )
    // but Modified() is Modified(item, aCopy, screen) -- so `screen` bound to
    // aCopy. SCH_SCREEN derives from EDA_ITEM, so this compiled silently while
    // handing the undo system the SCREEN as the symbol's "before" copy, which
    // corrupts the undo stack and crashes on push.
    //
    // Modify(item, screen) is what the rest of eeschema uses: it takes the
    // snapshot, and pulls the item from the screen's spatial index so its
    // geometry can safely change.
    SCH_COMMIT commit( frame->GetToolManager() );

    // Labels and no-connects are stored at pin coordinates, so moving only the
    // symbol strands them and quietly drops its pins off their nets. Capture
    // them first, move the part, then put each one back on the pin it marks.
    std::vector<PIN_MARKER> markers = collectPinMarkers( screen, target, currentSheet );

    for( const PIN_MARKER& marker : markers )
        commit.Modify( marker.item, screen );

    commit.Modify( target, screen );

    target->SetPosition( newPos );

    for( const PIN_MARKER& marker : markers )
    {
        marker.item->SetPosition( marker.pin->GetPosition() );
        frame->GetCanvas()->GetView()->Update( marker.item );
    }

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

    // Snapshot BEFORE mutating, using Modify() rather than Modified().
    //
    // The old code mutated the symbol first and then called
    //     commit.Modified( target, screen )
    // but Modified() is Modified(item, aCopy, screen) -- so `screen` bound to
    // aCopy. SCH_SCREEN derives from EDA_ITEM, so this compiled silently while
    // handing the undo system the SCREEN as the symbol's "before" copy, which
    // corrupts the undo stack and crashes on push.
    //
    // Modify(item, screen) is what the rest of eeschema uses: it takes the
    // snapshot, and pulls the item from the screen's spatial index so its
    // geometry can safely change.
    SCH_COMMIT commit( frame->GetToolManager() );

    // Rotating moves every pin, so the markers on them must move too. See
    // collectPinMarkers.
    std::vector<PIN_MARKER> markers = collectPinMarkers( screen, target, currentSheet );

    for( const PIN_MARKER& marker : markers )
        commit.Modify( marker.item, screen );

    commit.Modify( target, screen );

    for( int i = 0; i < steps; i++ )
        target->Rotate( center, true ); // true = counter-clockwise

    for( const PIN_MARKER& marker : markers )
    {
        marker.item->SetPosition( marker.pin->GetPosition() );
        frame->GetCanvas()->GetView()->Update( marker.item );
    }

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
    else if( toolName == "move_symbol" )
    {
        result = handleMoveSymbol( aPanel, args );
    }
    else if( toolName == "rotate_symbol" )
    {
        result = handleRotateSymbol( aPanel, args );
    }
    else if( toolName == "connect_net" )
    {
        result = handleConnectNet( aPanel, args );
    }
    else if( toolName == "redo_last" )
    {
        result = handleRedoLast( aPanel, args );
    }
    else if( toolName == "remove_no_connect" )
    {
        result = handleRemoveNoConnect( aPanel, args );
    }
    else if( toolName == "find_part" )
    {
        result = handleFindPart( aPanel, args );
    }
    else if( toolName == "check" )
    {
        result = handleCheck( aPanel, args );
    }
    else if( toolName == "undo_last" )
    {
        result = handleUndoLast( aPanel, args );
    }
    else if( toolName == "add_no_connects" )
    {
        result = handleAddNoConnects( aPanel, args );
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
// Tool: get_symbol_info
//
// Loads a symbol from KiCad's library WITHOUT placing it on the schematic.
// Returns complete component information: pins (name, number, electrical type,
// position), unit count, description, datasheet URL, footprint filters, and
// default reference prefix.
//
// The planner uses this to get exact pin information before building the plan,
// instead of relying on training knowledge which may be wrong.
//
// Arguments (JSON):
//   library_name — full symbol ID, e.g. "Device:R", "Timer:NE555P"
//
// Returns (JSON):
//   { "status": "ok", "library_name": "Device:LED", "name": "LED",
//     "description": "Light emitting diode", "datasheet": "",
//     "unit_count": 1, "default_ref": "D", "default_value": "LED",
//     "footprint_filters": ["LED_*"],
//     "pins": [ { "name": "A", "number": "1", "type": "passive", "unit": 1, "x": 0, "y": 0 }, ... ] }
// ---------------------------------------------------------------------------
static wxString handleGetSymbolInfo( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString libraryName = wxString::FromUTF8( aArgs.value( "library_name", "" ).c_str() );

    if( libraryName.IsEmpty() )
        return R"({ "error": "Missing 'library_name' argument. Example: Device:R" })";

    // Split "Library:Symbol" into parts
    int colonPos = libraryName.Find( ':' );

    if( colonPos == wxNOT_FOUND )
        return R"({ "error": "Invalid library_name format. Must be 'Library:Symbol', e.g. 'Device:R'" })";

    wxString libNickname = libraryName.Left( colonPos );
    wxString symName = libraryName.Mid( colonPos + 1 );

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &frame->Prj() );

    if( !adapter )
        return R"({ "error": "Symbol library adapter not available" })";

    LIB_SYMBOL* libSym = adapter->LoadSymbol( libNickname, symName );

    if( !libSym )
    {
        wxString err = wxString::Format( R"({ "error": "Symbol '%s' not found in library '%s'" })",
                                          symName.ToUTF8(), libNickname.ToUTF8() );
        return err;
    }

    json result;
    result["status"] = "ok";
    result["library_name"] = std::string( libraryName.ToUTF8() );
    result["name"] = std::string( libSym->GetName().ToUTF8() );
    result["description"] = std::string( libSym->GetDescription().ToUTF8() );
    result["datasheet"] = std::string( libSym->GetDatasheetField().GetText().ToUTF8() );
    result["unit_count"] = libSym->GetUnitCount();
    result["default_ref"] = std::string( libSym->GetReferenceField().GetText().ToUTF8() );
    result["default_value"] = std::string( libSym->GetValueField().GetText().ToUTF8() );

    // Footprint filters — tells which footprints are compatible
    wxArrayString fpFilters = libSym->GetFPFilters();
    json filtersArray = json::array();
    for( const wxString& filter : fpFilters )
        filtersArray.push_back( std::string( filter.ToUTF8() ) );
    result["footprint_filters"] = filtersArray;

    // Pins — name, number, electrical type, unit, position
    std::vector<SCH_PIN*> pins = libSym->GetPins();
    json pinsArray = json::array();

    for( SCH_PIN* pin : pins )
    {
        json pinEntry;
        pinEntry["name"] = std::string( pin->GetName().ToUTF8() );
        pinEntry["number"] = std::string( pin->GetNumber().ToUTF8() );

        // Electrical type as readable string
        ELECTRICAL_PINTYPE pinType = pin->GetType();
        wxString typeStr;

        switch( pinType )
        {
            case ELECTRICAL_PINTYPE::PT_INPUT:         typeStr = "input"; break;
            case ELECTRICAL_PINTYPE::PT_OUTPUT:        typeStr = "output"; break;
            case ELECTRICAL_PINTYPE::PT_BIDI:          typeStr = "bidirectional"; break;
            case ELECTRICAL_PINTYPE::PT_PASSIVE:       typeStr = "passive"; break;
            case ELECTRICAL_PINTYPE::PT_NIC:           typeStr = "not_internally_connected"; break;
            case ELECTRICAL_PINTYPE::PT_UNSPECIFIED:    typeStr = "unspecified"; break;
            case ELECTRICAL_PINTYPE::PT_POWER_IN:      typeStr = "power_in"; break;
            case ELECTRICAL_PINTYPE::PT_POWER_OUT:     typeStr = "power_out"; break;
            case ELECTRICAL_PINTYPE::PT_OPENCOLLECTOR: typeStr = "open_collector"; break;
            case ELECTRICAL_PINTYPE::PT_OPENEMITTER:   typeStr = "open_emitter"; break;
            case ELECTRICAL_PINTYPE::PT_NC:            typeStr = "no_connect"; break;
            default:                                    typeStr = "unknown"; break;
        }

        pinEntry["type"] = std::string( typeStr.ToUTF8() );
        pinEntry["unit"] = pin->GetUnit();

        VECTOR2I pinPos = pin->GetPosition();
        pinEntry["x"] = schIUScale.IUTomm( pinPos.x );
        pinEntry["y"] = schIUScale.IUTomm( pinPos.y );

        pinsArray.push_back( pinEntry );
    }

    result["pin_count"] = (int) pins.size();
    result["pins"] = pinsArray;

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: screenshot
//
// Captures the current eeschema canvas as a base64-encoded PNG image.
// The evaluator agent uses this to visually assess placement, overlap,
// and layout quality — things that JSON data alone cannot convey.
//
// Returns (JSON):
//   { "status": "ok", "image": "iVBORw0KGgo...", "width": 800, "height": 600 }
// ---------------------------------------------------------------------------
static wxString handleScreenshot( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_DRAW_PANEL* canvas = frame->GetCanvas();

    if( !canvas )
        return R"({ "error": "No canvas found" })";

    // Force a refresh so the screenshot reflects the latest state.
    //
    // Deliberately no wxYield() here. This runs inside a WebKit script message
    // handler, and yielding re-enters the event loop from there -- the exact
    // reentrancy that WEBVIEW_PANEL::DoInitHandlers() guards against because it
    // crashes JSC in sanitizeStackForVM. Update() paints synchronously without
    // pumping the event loop.
    canvas->Refresh();
    canvas->Update();

    // Get the canvas window and capture it
    // SCH_DRAW_PANEL derives from wxWindow (via EDA_DRAW_PANEL)
    wxWindow* canvasWin = dynamic_cast<wxWindow*>( canvas );

    if( !canvasWin )
        canvasWin = static_cast<wxWindow*>( canvas );

    if( !canvasWin )
        return R"({ "error": "No canvas window found" })";

    wxSize size = canvasWin->GetClientSize();

    if( size.GetWidth() <= 0 || size.GetHeight() <= 0 )
        return R"({ "error": "Canvas has invalid size" })";

    // Capture the canvas content into a bitmap
    wxBitmap bitmap( size );
    wxClientDC dc( canvasWin );
    wxMemoryDC memDC;

    memDC.SelectObject( bitmap );
    memDC.Blit( 0, 0, size.GetWidth(), size.GetHeight(), &dc, 0, 0 );
    memDC.SelectObject( wxNullBitmap );

    // Convert bitmap to PNG
    wxImage image = bitmap.ConvertToImage();

    if( !image.IsOk() )
        return R"({ "error": "Failed to convert bitmap to image" })";

    wxMemoryOutputStream stream;
    image.SaveFile( stream, wxBITMAP_TYPE_PNG );

    wxStreamBuffer* streamBuf = stream.GetOutputStreamBuffer();
    size_t dataSize = streamBuf->GetBufferSize();
    const unsigned char* data = static_cast<const unsigned char*>( streamBuf->GetBufferStart() );

    // Base64 encode
    static const char base64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string base64;
    base64.reserve( ( dataSize + 2 ) / 3 * 4 );

    for( size_t i = 0; i < dataSize; i += 3 )
    {
        unsigned int n = data[i] << 16;

        if( i + 1 < dataSize )
            n |= data[i + 1] << 8;
        if( i + 2 < dataSize )
            n |= data[i + 2];

        base64 += base64Chars[( n >> 18 ) & 0x3F];
        base64 += base64Chars[( n >> 12 ) & 0x3F];
        base64 += ( i + 1 < dataSize ) ? base64Chars[( n >> 6 ) & 0x3F ] : '=';
        base64 += ( i + 2 < dataSize ) ? base64Chars[n & 0x3F] : '=';
    }

    json result;
    result["status"] = "ok";
    result["image"] = base64;
    result["width"] = size.GetWidth();
    result["height"] = size.GetHeight();
    result["format"] = "png";

    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: get_schematic
//
// Returns all symbols currently on the schematic with their properties.
//
// Returns (JSON):
//   { "count": 3,
//     "components": [ { "ref": "R1", ..., "pins": [ { ..., "net": "+5V" } ] } ],
//     "wires":      [ { "from": {"x":..,"y":..,"ref":"R1","pin":"1"}, "to": {...} } ],
//     "labels":     [ { "text": "+5V", "x": .., "y": .., "ref": "R1", "pin": "1" } ],
//     "junctions":  [ { "x": .., "y": .. } ],
//     "no_connects":[ { "x": .., "y": .., "ref": "U1", "pin": "3" } ],
//     "nets":       [ { "name": "+5V", "pins": [ "R1.1", "U1.8" ] } ] }
//
// The "nets" array is the authoritative connectivity answer — it comes from
// KiCad's own connection graph, not from tracing wire geometry. Two pins are
// electrically connected if and only if they share a net name. Use this rather
// than trying to follow "wires" by hand.
// ---------------------------------------------------------------------------

// Helper: if a point coincides with a symbol pin, record which ref/pin it is.
static void annotatePointWithPin( SCH_SCREEN* aScreen, SCH_SHEET_PATH& aSheet,
                                  const VECTOR2I& aPos, json& aTarget )
{
    for( SCH_ITEM* item : aScreen->Items().OfType( SCH_SYMBOL_T ) )
    {
        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

        for( SCH_PIN* pin : symbol->GetPins( &aSheet ) )
        {
            if( pin->GetPosition() == aPos )
            {
                aTarget["ref"] = std::string( symbol->GetRef( &aSheet, true ).ToUTF8() );
                aTarget["pin"] = std::string( pin->GetNumber().ToUTF8() );
                aTarget["pin_name"] = std::string( pin->GetName().ToUTF8() );
                return;
            }
        }
    }
}


static wxString handleGetSchematic( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    SCH_SCREEN* screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    // Build the connection graph so per-pin net names are current. Without this
    // the "net" fields below are stale (or empty) right after a wiring change,
    // which is exactly when the caller needs them. LOCAL_CLEANUP is used rather
    // than GLOBAL_CLEANUP so this read-only tool does not merge collinear wires
    // and destroy junctions that connect_pins just created.
    if( SCHEMATIC* sch = &frame->Schematic() )
    {
        SCH_COMMIT recalcCommit( frame->GetToolManager() );
        sch->RecalculateConnections( &recalcCommit, LOCAL_CLEANUP, frame->GetToolManager() );

        // Only push if something actually changed, so a plain read does not
        // pollute the undo stack with empty entries.
        if( !recalcCommit.Empty() )
            recalcCommit.Push( _( "Recalculate (AI Read)" ) );
    }

    // Accumulates net name -> list of "REF.pin" as we walk the pins below.
    std::map<wxString, std::vector<wxString>> netMap;

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

            // The net this pin belongs to, straight from the connection graph.
            // This is the ground truth for "is this pin connected to that one" —
            // two pins are connected exactly when their net names match.
            wxString netName;

            if( SCH_CONNECTION* conn = pin->Connection( &currentSheet ) )
                netName = conn->Name( true );

            pinEntry["net"] = std::string( netName.ToUTF8() );

            if( !netName.IsEmpty() )
                netMap[netName].push_back( ref + wxS( "." ) + pin->GetNumber() );

            pinsArray.push_back( pinEntry );
        }

        entry["pins"] = pinsArray;

        componentsArray.push_back( entry );
        count++;
    }

    // ── Wires ──
    // Each wire endpoint is annotated with the ref/pin it lands on (if any) so
    // the caller does not have to match floating point coordinates itself.
    json wiresArray = json::array();

    for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
    {
        if( item->GetLayer() != LAYER_WIRE )
            continue;

        SCH_LINE* line = static_cast<SCH_LINE*>( item );
        VECTOR2I  start = line->GetStartPoint();
        VECTOR2I  end   = line->GetEndPoint();

        json from;
        from["x"] = schIUScale.IUTomm( start.x );
        from["y"] = schIUScale.IUTomm( start.y );
        annotatePointWithPin( screen, currentSheet, start, from );

        json to;
        to["x"] = schIUScale.IUTomm( end.x );
        to["y"] = schIUScale.IUTomm( end.y );
        annotatePointWithPin( screen, currentSheet, end, to );

        json wireEntry;
        wireEntry["from"] = from;
        wireEntry["to"] = to;
        wiresArray.push_back( wireEntry );
    }

    // ── Labels ──
    // Local, global and hierarchical labels all carry net names, so all three
    // types are reported. connect_label can create local or global labels.
    json labelsArray = json::array();

    const KICAD_T labelTypes[] = { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T };

    for( KICAD_T labelType : labelTypes )
    {
        for( SCH_ITEM* item : screen->Items().OfType( labelType ) )
        {
            SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
            VECTOR2I        pos = label->GetPosition();

            json labelEntry;
            labelEntry["text"] = std::string( label->GetText().ToUTF8() );
            labelEntry["type"] = ( labelType == SCH_GLOBAL_LABEL_T ) ? "global"
                                 : ( labelType == SCH_HIER_LABEL_T ) ? "hierarchical"
                                                                     : "local";
            labelEntry["x"] = schIUScale.IUTomm( pos.x );
            labelEntry["y"] = schIUScale.IUTomm( pos.y );
            annotatePointWithPin( screen, currentSheet, pos, labelEntry );

            labelsArray.push_back( labelEntry );
        }
    }

    // ── Junctions ──
    json junctionsArray = json::array();

    for( SCH_ITEM* item : screen->Items().OfType( SCH_JUNCTION_T ) )
    {
        VECTOR2I pos = item->GetPosition();

        json junctionEntry;
        junctionEntry["x"] = schIUScale.IUTomm( pos.x );
        junctionEntry["y"] = schIUScale.IUTomm( pos.y );
        junctionsArray.push_back( junctionEntry );
    }

    // ── No-connects ──
    json noConnectsArray = json::array();

    for( SCH_ITEM* item : screen->Items().OfType( SCH_NO_CONNECT_T ) )
    {
        VECTOR2I pos = item->GetPosition();

        json ncEntry;
        ncEntry["x"] = schIUScale.IUTomm( pos.x );
        ncEntry["y"] = schIUScale.IUTomm( pos.y );
        annotatePointWithPin( screen, currentSheet, pos, ncEntry );

        noConnectsArray.push_back( ncEntry );
    }

    // ── Nets ──
    json netsArray = json::array();

    for( const auto& [netName, pinRefs] : netMap )
    {
        json netEntry;
        netEntry["name"] = std::string( netName.ToUTF8() );

        json pinList = json::array();

        for( const wxString& pinRef : pinRefs )
            pinList.push_back( std::string( pinRef.ToUTF8() ) );

        netEntry["pins"] = pinList;
        netEntry["pin_count"] = (int) pinRefs.size();
        netsArray.push_back( netEntry );
    }

    json result;
    result["count"] = count;
    result["components"] = componentsArray;
    result["wires"] = wiresArray;
    result["labels"] = labelsArray;
    result["junctions"] = junctionsArray;
    result["no_connects"] = noConnectsArray;
    result["nets"] = netsArray;

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

    // Everything sitting on this symbol's pins goes with it. Gather before
    // removing, while the pins still have positions.
    std::vector<PIN_MARKER> markers = collectPinMarkers( screen, target, currentSheet );

    SCH_COMMIT commit( frame->GetToolManager() );

    json removedLabels = json::array();

    for( const PIN_MARKER& marker : markers )
    {
        if( SCH_LABEL_BASE* label = dynamic_cast<SCH_LABEL_BASE*>( marker.item ) )
            removedLabels.push_back( std::string( label->GetText().ToUTF8() ) );

        frame->RemoveFromScreen( marker.item, screen );
        commit.Removed( marker.item, screen );
        frame->GetCanvas()->GetView()->Remove( marker.item );
    }

    frame->RemoveFromScreen( target, screen );
    commit.Removed( target, screen );

    // One commit for the part and its markers, so a single undo_last brings the
    // whole thing back rather than resurrecting the symbol without its nets.
    commit.Push( _( "Delete Symbol (AI)" ) );

    frame->GetCanvas()->GetView()->Remove( target );
    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["deleted"] = std::string( refToDelete.ToUTF8() );

    if( !removedLabels.empty() )
    {
        result["removed_labels"] = removedLabels;
        result["note"] = "The net labels on this part's pins were removed with it. Any net "
                         "that relied on them is now shorter -- re-check it.";
    }

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

// ---------------------------------------------------------------------------
// Tool: redo_last
//
// Restores what undo_last reversed. KiCad has always had redo; it was simply
// never exposed, which meant an undo that went too far destroyed work
// permanently. Pairing the two makes over-undoing recoverable rather than fatal.
// ---------------------------------------------------------------------------
static wxString handleRedoLast( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    int steps = aArgs.value( "steps", 1 );

    if( steps < 1 )
        steps = 1;

    int redone = 0;

    for( int i = 0; i < steps; i++ )
    {
        if( !frame->GetScreen() || frame->GetRedoCommandCount() == 0 )
            break;

        frame->GetToolManager()->RunAction( ACTIONS::redo );
        redone++;
    }

    if( redone == 0 )
        return R"({ "error": "Nothing to redo" })";

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["redone"] = redone;
    return wxString::FromUTF8( result.dump().c_str() );
}


// ---------------------------------------------------------------------------
// Tool: remove_no_connect
//
// Clears the "deliberately unused" marker from a pin so it can join a net.
//
// Without this the caller could reach a dead end: a pin marked unused could not
// be connected, and the error telling it to remove the marker named an action no
// tool could perform. It retried instead, until the round ran out.
// ---------------------------------------------------------------------------
static wxString handleRemoveNoConnect( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString ref = wxString::FromUTF8( aArgs.value( "ref", "" ).c_str() );
    wxString pinId = wxString::FromUTF8( aArgs.value( "pin", "" ).c_str() );

    if( ref.IsEmpty() || pinId.IsEmpty() )
        return R"({ "error": "Missing 'ref' or 'pin'" })";

    SCH_SCREEN*     screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    SCH_PIN* pin = findPinByRef( screen, currentSheet, ref, pinId );

    if( !pin )
        return wxString::Format( R"({ "error": "Pin not found: %s pin %s" })", ref, pinId );

    VECTOR2I pinPos = pin->GetPosition();

    SCH_NO_CONNECT* target = nullptr;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_NO_CONNECT_T ) )
    {
        if( item->GetPosition() == pinPos )
        {
            target = static_cast<SCH_NO_CONNECT*>( item );
            break;
        }
    }

    if( !target )
    {
        // Already in the desired state. Reporting success rather than an error
        // matters: an error here invites a retry of something already true.
        json ok;
        ok["status"] = "ok";
        ok["removed"] = false;
        ok["note"] = "That pin had no unused marker — nothing to remove.";
        return wxString::FromUTF8( ok.dump().c_str() );
    }

    SCH_COMMIT commit( frame->GetToolManager() );
    commit.Removed( target, screen );
    frame->RemoveFromScreen( target, screen );
    frame->GetCanvas()->GetView()->Remove( target );
    commit.Push( _( "Remove No-Connect (AI)" ) );

    if( SCHEMATIC* sch = &frame->Schematic() )
    {
        SCH_COMMIT recalcCommit( frame->GetToolManager() );
        sch->RecalculateConnections( &recalcCommit, LOCAL_CLEANUP, frame->GetToolManager() );

        if( !recalcCommit.Empty() )
            recalcCommit.Push( _( "Recalculate (AI)" ) );
    }

    frame->GetCanvas()->Refresh();

    json result;
    result["status"] = "ok";
    result["removed"] = true;
    result["ref"] = std::string( ref.ToUTF8() );
    result["pin"] = std::string( pin->GetNumber().ToUTF8() );
    return wxString::FromUTF8( result.dump().c_str() );
}

// ---------------------------------------------------------------------------
// Tool: find_part
//
// Replaces search_symbols + get_symbol_info, which were never used apart: you
// searched to find a symbol id, then immediately asked what its pins were. Two
// round trips, and when the second was skipped the caller had no footprint and
// invented one.
//
// Returning both together is also SMALLER, because full detail is only needed
// for the part actually being placed. Each candidate carries just enough to
// judge it — pin names and a footprint count — and the best match carries the
// real footprint ids to copy.
//
// The footprint count is the important signal. A symbol with none is not a
// physical part: it is a simulation or documentation symbol, and no amount of
// name matching makes it buildable. That single number distinguishes
// Switch:SW_Push from Simulation_SPICE:SWITCH, and a real resistor from
// Device:VoltageDivider, without any hardcoded list of parts to avoid.
// ---------------------------------------------------------------------------
static wxString handleFindPart( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    wxString rawQuery = wxString::FromUTF8( aArgs.value( "query", "" ).c_str() );

    if( rawQuery.IsEmpty() )
        return R"({ "error": "Missing 'query'. Say what the part is, e.g. \"resistor\"." })";

    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &frame->Prj() );

    if( !adapter )
        return R"({ "error": "Symbol library not available" })";

    // An exact "Library:Symbol" id means the caller already knows what it wants,
    // so skip the search entirely and describe just that part.
    const bool exactId = rawQuery.Contains( wxS( ":" ) );

    struct Candidate
    {
        wxString libId;
        wxString libNickname;
        wxString symName;
        int      relevance;
    };

    std::vector<Candidate> candidates;

    if( exactId )
    {
        candidates.push_back( { rawQuery,
                                rawQuery.BeforeFirst( ':' ),
                                rawQuery.AfterFirst( ':' ),
                                0 } );
    }
    else
    {
        wxString query = rawQuery;
        query.LowerCase();

        // Name and library matching only — no symbol is loaded from disk here.
        // Loading every symbol to search descriptions meant ~23,000 file reads
        // per call, which blew the tool timeout and killed runs before they ever
        // reached wiring.
        for( const wxString& libNickname : adapter->GetLibraryNames() )
        {
            wxString lowerLib = libNickname;
            lowerLib.LowerCase();

            for( const wxString& symName : adapter->GetSymbolNames( libNickname ) )
            {
                wxString lowerSym = symName;
                lowerSym.LowerCase();

                int relevance = -1;

                if( lowerSym == query )                 relevance = 0;
                else if( lowerSym.StartsWith( query ) ) relevance = 1;
                else if( lowerSym.Contains( query ) )   relevance = 2;
                else if( lowerLib.Contains( query ) )   relevance = 3;
                else                                    continue;

                candidates.push_back( { libNickname + wxS( ":" ) + symName,
                                        libNickname, symName, relevance } );
            }
        }

        std::stable_sort( candidates.begin(), candidates.end(),
                          []( const Candidate& a, const Candidate& b )
                          { return a.relevance < b.relevance; } );
    }

    if( candidates.empty() )
    {
        json none;
        none["status"] = "ok";
        none["count"] = 0;
        none["parts"] = json::array();
        none["note"] = "Nothing matched. Search for the COMPONENT rather than the "
                       "circuit — a divider is two resistors, so search \"resistor\".";
        return wxString::FromUTF8( none.dump().c_str() );
    }

    const size_t MAX_CANDIDATES = 5;
    const size_t shown = std::min( candidates.size(), MAX_CANDIDATES );

    json parts = json::array();

    for( size_t i = 0; i < shown; i++ )
    {
        const Candidate& c = candidates[i];

        LIB_SYMBOL* sym = adapter->LoadSymbol( c.libNickname, c.symName );

        if( !sym )
            continue;

        json entry;
        entry["symbol"] = std::string( c.libId.ToUTF8() );
        entry["description"] = std::string( sym->GetDescription().ToUTF8() );

        // Pins: names and numbers are short, and they are what distinguishes a
        // real part from a lookalike. A plain resistor has two unnamed pins; a
        // potentiometer has three.
        json pins = json::array();

        for( SCH_PIN* pin : sym->GetPins() )
        {
            json p;
            p["number"] = std::string( pin->GetNumber().ToUTF8() );
            p["name"] = std::string( pin->GetName().ToUTF8() );
            p["type"] = std::string( pin->GetElectricalTypeName().ToUTF8() );
            pins.push_back( p );
        }

        entry["pins"] = pins;
        entry["pin_count"] = (int) pins.size();

        // Real footprints for this symbol, resolved from its own filters.
        std::vector<wxString> footprints;

        for( const wxString& filter : sym->GetFPFilters() )
        {
            wxString pattern = filter;
            pattern.Replace( wxS( "*" ), wxEmptyString );
            pattern.Replace( wxS( "?" ), wxEmptyString );

            if( pattern.Contains( wxS( ":" ) ) )
                pattern = pattern.AfterLast( ':' );

            pattern.LowerCase();

            LIBRARY_MANAGER& libMgr = Pgm().GetLibraryManager();

            for( const LIBRARY_TABLE_ROW* row : libMgr.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
            {
                wxString uri = ExpandEnvVarSubstitutions( row->URI(), &frame->Prj() );

                if( uri.IsEmpty() || !wxDirExists( uri ) )
                    continue;

                wxDir dir( uri );

                if( !dir.IsOpened() )
                    continue;

                wxString fileName;
                bool     more = dir.GetFirst( &fileName, wxS( "*.kicad_mod" ), wxDIR_FILES );

                while( more && footprints.size() < 40 )
                {
                    wxString fpName = fileName.BeforeLast( '.' );
                    wxString lowerFp = fpName;
                    lowerFp.LowerCase();

                    if( pattern.IsEmpty() || lowerFp.StartsWith( pattern ) )
                        footprints.push_back( row->Nickname() + wxS( ":" ) + fpName );

                    more = dir.GetNext( &fileName );
                }
            }
        }

        entry["footprint_count"] = (int) footprints.size();

        // Full ids only for the best match — the rest just need the count so the
        // caller can tell a real part from one that cannot be built.
        if( i == 0 )
        {
            json fps = json::array();

            for( size_t f = 0; f < std::min<size_t>( footprints.size(), 8 ); f++ )
                fps.push_back( std::string( footprints[f].ToUTF8() ) );

            entry["footprints"] = fps;
        }

        if( footprints.empty() )
        {
            entry["warning"] = "No footprints exist for this symbol, so it is not a "
                               "physical part — it is for simulation or documentation "
                               "and cannot be built.";
        }

        parts.push_back( entry );
    }

    json result;
    result["status"] = "ok";
    result["count"] = (int) parts.size();
    result["total_matches"] = (int) candidates.size();
    result["parts"] = parts;

    return wxString::FromUTF8( result.dump().c_str() );
}

// ---------------------------------------------------------------------------
// Tool: check
//
// Answers "is this schematic sound?" — the question the caller needs after
// building or changing anything, and the one an LLM used to be asked to answer
// by eye. It could not: a language model reading a netlist invented wiring
// errors on circuits that were electrically perfect. Correctness here is
// decidable, so it is decided in code.
//
// Runs KiCad's own electrical rule check, and adds what that check does not
// cover: footprints that do not exist, parts sitting on top of each other, and
// duplicate references. Those are all reasons a board cannot be built, so they
// belong in the same answer rather than in a separate tool the caller might not
// think to call.
//
// Footprint problems are ONE category. Missing, invented and wildcarded all mean
// the same thing to the reader — this part cannot be manufactured — and each
// comes with real footprints that would work.
// ---------------------------------------------------------------------------
static wxString handleCheck( AI_ASSISTANT_PANEL* aPanel, const json& aArgs )
{
    SCH_EDIT_FRAME* frame = getSchEditFrame( aPanel );

    if( !frame )
        return R"({ "error": "No schematic editor found" })";

    json problems = json::array();
    json warnings = json::array();

    // ── KiCad's electrical rule check ──
    wxString ercRaw = handleRunErc( aPanel, json::object() );
    json     erc;

    try
    {
        erc = json::parse( std::string( ercRaw.ToUTF8() ) );
    }
    catch( ... )
    {
        erc = json::object();
    }

    if( erc.contains( "markers" ) )
    {
        for( const auto& marker : erc["markers"] )
        {
            json entry;
            entry["kind"] = "electrical";
            entry["message"] = marker.value( "message", "" );

            if( marker.contains( "ref" ) )
            {
                entry["ref"] = marker["ref"];
                entry["pin"] = marker.value( "pin", "" );

                if( marker.contains( "pin_name" ) )
                    entry["pin_name"] = marker["pin_name"];
            }

            if( marker.value( "severity", "" ) == "error" )
                problems.push_back( entry );
            else
                warnings.push_back( entry );
        }
    }

    SCH_SCREEN*     screen = frame->GetScreen();
    SCH_SHEET_PATH& currentSheet = frame->GetCurrentSheet();

    std::vector<SCH_SYMBOL*> symbols;

    for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
        symbols.push_back( static_cast<SCH_SYMBOL*>( item ) );

    // ── Footprints ──
    // Gather real footprint names once, so an invalid one can be answered with
    // working alternatives rather than just rejected.
    std::set<wxString> realFootprints;
    {
        LIBRARY_MANAGER& libMgr = Pgm().GetLibraryManager();

        for( const LIBRARY_TABLE_ROW* row : libMgr.Rows( LIBRARY_TABLE_TYPE::FOOTPRINT ) )
        {
            wxString uri = ExpandEnvVarSubstitutions( row->URI(), &frame->Prj() );

            if( uri.IsEmpty() || !wxDirExists( uri ) )
                continue;

            wxDir dir( uri );

            if( !dir.IsOpened() )
                continue;

            wxString fileName;
            bool     more = dir.GetFirst( &fileName, wxS( "*.kicad_mod" ), wxDIR_FILES );

            while( more )
            {
                realFootprints.insert( row->Nickname() + wxS( ":" ) + fileName.BeforeLast( '.' ) );
                more = dir.GetNext( &fileName );
            }
        }
    }

    for( SCH_SYMBOL* sym : symbols )
    {
        wxString ref = sym->GetRef( &currentSheet, true );
        wxString fp = sym->GetFootprintFieldText( true, &currentSheet, false );

        bool bad = fp.IsEmpty() || !fp.Contains( wxS( ":" ) )
                   || fp.Contains( wxS( "*" ) ) || fp.Contains( wxS( "?" ) )
                   || !realFootprints.count( fp );

        if( !bad )
            continue;

        json entry;
        entry["kind"] = "footprint";
        entry["ref"] = std::string( ref.ToUTF8() );
        entry["message"] = fp.IsEmpty()
                ? std::string( "has no footprint" )
                : ( "footprint \"" + std::string( fp.ToUTF8() ) + "\" does not exist" );

        // Offer real options, taken from this symbol's own filters.
        json options = json::array();

        if( LIB_SYMBOL* libSym = sym->GetLibSymbolRef().get() )
        {
            for( const wxString& filter : libSym->GetFPFilters() )
            {
                wxString pattern = filter;
                pattern.Replace( wxS( "*" ), wxEmptyString );
                pattern.Replace( wxS( "?" ), wxEmptyString );

                if( pattern.Contains( wxS( ":" ) ) )
                    pattern = pattern.AfterLast( ':' );

                pattern.LowerCase();

                for( const wxString& real : realFootprints )
                {
                    if( options.size() >= 3 )
                        break;

                    wxString name = real.AfterFirst( ':' );
                    name.LowerCase();

                    if( pattern.IsEmpty() || name.StartsWith( pattern ) )
                        options.push_back( std::string( real.ToUTF8() ) );
                }

                if( options.size() >= 3 )
                    break;
            }
        }

        if( !options.empty() )
            entry["try"] = options;

        problems.push_back( entry );
    }

    // ── Overlapping parts ──
    // Two symbols on top of each other are unreadable, and usually mean a
    // position was guessed rather than chosen.
    const double OVERLAP_MM = 8.0;

    for( size_t i = 0; i < symbols.size(); i++ )
    {
        for( size_t j = i + 1; j < symbols.size(); j++ )
        {
            VECTOR2I a = symbols[i]->GetPosition();
            VECTOR2I b = symbols[j]->GetPosition();

            double dx = schIUScale.IUTomm( std::abs( a.x - b.x ) );
            double dy = schIUScale.IUTomm( std::abs( a.y - b.y ) );

            if( dx >= OVERLAP_MM || dy >= OVERLAP_MM )
                continue;

            json entry;
            entry["kind"] = "layout";
            entry["ref"] = std::string( symbols[i]->GetRef( &currentSheet, true ).ToUTF8() );
            entry["message"] = "overlaps "
                    + std::string( symbols[j]->GetRef( &currentSheet, true ).ToUTF8() );
            warnings.push_back( entry );
        }
    }

    // ── Stray labels ──
    // A label attached to nothing names no net. ERC reports it as "Label not
    // connected", which reads like a wiring fault and sends the reader hunting
    // for a break that is not there.
    for( KICAD_T type : { SCH_LABEL_T, SCH_GLOBAL_LABEL_T, SCH_HIER_LABEL_T } )
    {
        for( SCH_ITEM* item : screen->Items().OfType( type ) )
        {
            SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );

            if( hasConnectableAt( screen, currentSheet, label->GetPosition() ) )
                continue;

            json entry;
            entry["type"] = "stray_label";
            entry["net"] = std::string( label->GetText().ToUTF8() );
            entry["at"]["x"] = schIUScale.IUTomm( label->GetPosition().x );
            entry["at"]["y"] = schIUScale.IUTomm( label->GetPosition().y );
            entry["message"] = wxString::Format(
                    "Label \"%s\" sits on no pin and no wire, so it is not part of any net. "
                    "Re-issue connect_net for \"%s\" and it will be cleared.",
                    label->GetText(), label->GetText() ).ToStdString();

            problems.push_back( entry );
        }
    }

    // ── Duplicate references ──
    // Two parts sharing a reference make the netlist ambiguous and the bill of
    // materials wrong.
    std::map<wxString, int> refCounts;

    for( SCH_SYMBOL* sym : symbols )
        refCounts[sym->GetRef( &currentSheet, true )]++;

    for( const auto& [ref, count] : refCounts )
    {
        if( count < 2 )
            continue;

        json entry;
        entry["kind"] = "reference";
        entry["ref"] = std::string( ref.ToUTF8() );
        entry["message"] = std::to_string( count ) + " parts share this reference";
        problems.push_back( entry );
    }

    json result;
    result["status"] = "ok";
    result["ok"] = problems.empty();
    result["component_count"] = (int) symbols.size();
    result["problems"] = problems;
    result["warnings"] = warnings;

    if( problems.empty() && warnings.empty() )
        result["note"] = "Nothing wrong found.";

    return wxString::FromUTF8( result.dump().c_str() );
}
