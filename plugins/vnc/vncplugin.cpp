/*
    Shellspace - One tiny step towards the VR Desktop Operating System
    Copyright (C) 2015  Wade Brainerd

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "common.h"
#include "vncplugin.h"
#include "message.h"
#include "registry.h"
#include "thread.h"

#include <android/keycodes.h>
#include <ctype.h>
#include <libvncserver/rfb/rfbclient.h>


#define VNC_WIDGET_LIMIT 			16

#define AKEYCODE_UNKNOWN 			(UINT_MAX)
#define INVALID_KEY_CODE            (-1)


enum EVNCState
{
	VNCSTATE_DISCONNECTED,
	VNCSTATE_CONNECTING,
	VNCSTATE_CONNECTED
};


struct SVNCCursor
{
	sbool 				sendUpdates;
	uint 				buttons;
	uint 				xPos;
	uint 				yPos;
	uint 				xHot;
	uint 				yHot;
	uint 				width;
	uint 				height;
};


struct SVNCWidget
{
	SxWidgetHandle 		id;
	SxEntityHandle 		cursorId;

	pthread_t 			thread;
	volatile sbool 		disconnect;

	SMsgQueue			msgQueue;

	rfbClient    		*client;
	char 				*server;
	char 				*password;

	volatile EVNCState	state;

	SVNCCursor			cursor;

	int 				width;
	int 				height;
	sbool 				updatePending;

	float 				latArc;
	float 				lonArc;
	float 				depth;
};


struct SVNCKeyMap
{
	const char 			*name;
	uint 				androidCode;
	uint 				vncCode;
};


struct SVNCGlobals
{
	sbool 				initialized;

	sbool 				headmouse;

	SVNCWidget 			widgetPool[VNC_WIDGET_LIMIT];

	pthread_t 			pluginThread;
};


static SVNCGlobals s_vncGlob;


static void rfb_log(const char *format, ...)
{
    va_list args;
    char msg[256];

    va_start( args, format );
    vsnprintf( msg, sizeof( msg ) - 1, format, args );
    va_end( args );

    S_Log( "%s", msg );
}


static void rfb_error(const char *format, ...)
{
    va_list args;
    char 	msg[256];
    uint 	msgPos;

    msgPos = 0;
    
    S_sprintfPos( msg, sizeof( msg ), &msgPos, "notify \"" );

    va_start( args, format );
    S_vsprintfPos( msg, sizeof( msg ) - 1, &msgPos, format, args );
    va_end( args );

    S_sprintfPos( msg, sizeof( msg ), &msgPos, "\"" );

    S_Log( "%s", msg );

    g_pluginInterface.postMessage( msg );
}


static char *vnc_thread_get_password( rfbClient *client )
{
	SVNCWidget 	*vnc;

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	return strdup( vnc->password );
}


void VNCThread_GetGlobePosition( SVNCWidget *vnc, float u, float v, SxVector3 *result )
{
	float lon;

	lon = S_degToRad( (u - 0.5f) * vnc->lonArc ) - S_PI/2;

	result->x = vnc->depth * cosf( lon );
	result->y = vnc->depth * (v - 0.5f) * sinf( S_degToRad( vnc->latArc ) );
	result->z = vnc->depth * sinf( lon ) + vnc->depth;
}


void VNCThread_RebuildGeometry( SVNCWidget *vnc )
{
	char 	msg[256];
	float 	aspect;

	assert( vnc );

	if ( vnc->width && vnc->height )
		aspect = (float)vnc->width / vnc->height;
	else
		aspect = 16.0f / 10.0f;

	if ( vnc->lonArc > vnc->latArc * aspect )
		vnc->lonArc = vnc->latArc * aspect;

	snprintf( msg, sizeof( msg ), "shell make rect %s %f %f %f", vnc->id, vnc->latArc, vnc->lonArc, vnc->depth );

	g_pluginInterface.postMessage( msg );
}


void VNCThread_BuildCursorGeometry( SVNCWidget *vnc )
{
	SxVector3 	positions[4];
	SxVector2 	texCoords[4];
	SxColor 	colors[4];
	ushort 		indices[6];

	g_pluginInterface.sizeGeometry( vnc->cursorId, 4, 6 );

	Vec3Set( &positions[0], 0.0f, 0.0f, 0.0f );
	Vec3Set( &positions[1], 0.0f, 0.0f, 0.0f );
	Vec3Set( &positions[2], 0.0f, 0.0f, 0.0f );
	Vec3Set( &positions[3], 0.0f, 0.0f, 0.0f );

	Vec2Set( &texCoords[0], 0.0f, 1.0f );
	Vec2Set( &texCoords[1], 1.0f, 1.0f );
	Vec2Set( &texCoords[2], 0.0f, 0.0f );
	Vec2Set( &texCoords[3], 1.0f, 0.0f );

	ColorSet( &colors[0], 255, 255, 255, 255 );
	ColorSet( &colors[1], 255, 255, 255, 255 );
	ColorSet( &colors[2], 255, 255, 255, 255 );
	ColorSet( &colors[3], 255, 255, 255, 255 );

	indices[0] = 0;
	indices[1] = 1;
	indices[2] = 2;
	indices[3] = 2;
	indices[4] = 1;
	indices[5] = 3;

	g_pluginInterface.updateGeometryPositionRange( vnc->cursorId, 0, 4, positions );
	g_pluginInterface.updateGeometryTexCoordRange( vnc->cursorId, 0, 4, texCoords );
	g_pluginInterface.updateGeometryColorRange( vnc->cursorId, 0, 4, colors );
	g_pluginInterface.updateGeometryIndexRange( vnc->cursorId, 0, 6, indices );
	g_pluginInterface.presentGeometry( vnc->cursorId );
}


void VNCThread_BuildCursorTexture( SVNCWidget *vnc )
{
	g_pluginInterface.formatTexture( vnc->cursorId, SxTextureFormat_R8G8B8A8 );
	g_pluginInterface.sizeTexture( vnc->cursorId, 9, 9 );

	uint cursorPixels[9 * 9] = 
	{
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xff000000, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xffffffff, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xffffffff, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
		0xff000000, 0xff000000, 0xff000000, 0xff000000, 0xffffffff, 0xff000000, 0xff000000, 0xff000000, 0xff000000, 
		0xff000000, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xff000000, 
		0xff000000, 0xff000000, 0xff000000, 0xff000000, 0xffffffff, 0xff000000, 0xff000000, 0xff000000, 0xff000000, 
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xffffffff, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xffffffff, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
		0x00000000, 0x00000000, 0x00000000, 0xff000000, 0xff000000, 0xff000000, 0x00000000, 0x00000000, 0x00000000, 
	};

	g_pluginInterface.updateTextureRect( vnc->cursorId, 0, 0, 9, 9, 9 * 4, cursorPixels );
	g_pluginInterface.presentTexture( vnc->cursorId );
}


void VNCThread_BuildCursor( SVNCWidget *vnc )
{
	char 			cursorBuf[ID_LIMIT];
	SxOrientation	orient;
	SxTrajectory 	tr;

	snprintf( cursorBuf, ID_LIMIT, "%s_cursor", vnc->id );
	vnc->cursorId = strdup( cursorBuf );

	vnc->cursor.sendUpdates = strue;
	vnc->cursor.width = 9;
	vnc->cursor.height = 9;
	vnc->cursor.xHot = 4;
	vnc->cursor.yHot = 4;

	g_pluginInterface.registerEntity( vnc->cursorId );

	g_pluginInterface.registerTexture( vnc->cursorId );
	g_pluginInterface.setEntityTexture( vnc->cursorId, vnc->cursorId );

	VNCThread_BuildCursorTexture( vnc );

	g_pluginInterface.registerGeometry( vnc->cursorId );
	g_pluginInterface.setEntityGeometry( vnc->cursorId, vnc->cursorId );

	VNCThread_BuildCursorGeometry( vnc );

	tr.kind = SxTrajectoryKind_Instant;

	IdentityOrientation( &orient );
	orient.origin.z += 0.01f;

	g_pluginInterface.orientEntity( vnc->cursorId, &orient, &tr );
}


void VNC_SetCursorPos( SVNCWidget *vnc, int x, int y )
{
	SxVector3 	positions[4];
	int 		cursorX;
	int 		cursorY;
	float 		cursorLeft;
	float 		cursorRight;
	float 		cursorTop;
	float 		cursorBottom;

	vnc->cursor.xPos = x;
	vnc->cursor.yPos = y;

	cursorX = (int)x - vnc->cursor.xHot;
	cursorY = (int)y - vnc->cursor.yHot;

	cursorLeft = (float)cursorX / vnc->width;
	cursorRight = (float)(cursorX + vnc->cursor.width) / vnc->width;

	cursorTop = 1.0f - (float)(cursorY + vnc->cursor.height) / vnc->height;
	cursorBottom = 1.0f - (float)cursorY / vnc->height;

	VNCThread_GetGlobePosition( vnc, cursorLeft, cursorTop, &positions[0] );
	VNCThread_GetGlobePosition( vnc, cursorRight, cursorTop, &positions[1] );
	VNCThread_GetGlobePosition( vnc, cursorLeft, cursorBottom, &positions[2] );
	VNCThread_GetGlobePosition( vnc, cursorRight, cursorBottom, &positions[3] );

	g_pluginInterface.updateGeometryPositionRange( vnc->cursorId, 0, 4, positions );
	g_pluginInterface.presentGeometry( vnc->cursorId );
}


rfbBool vnc_thread_resize( rfbClient *client ) 
{
	SVNCWidget 		*vnc;
	int 			width;
	int 			height;

	Prof_Start( PROF_VNC_THREAD_HANDLE_RESIZE );

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	width = client->width;
	height = client->height;

	S_Log( "Requested client dimensions: %dx%d", width, height );

	client->updateRect.x = 0;
	client->updateRect.y = 0;
	client->updateRect.w = width; 
	client->updateRect.h = height;

	S_Log( "Frame buffer size          : %d", width * height * 4 );

	if ( client->frameBuffer )
		free( client->frameBuffer );

	client->frameBuffer = (uint8_t *)malloc( width * height * 4 );
	if ( !client->frameBuffer )
		S_Fail( "Failed to allocate %dx%dx32bpp client frame buffer.", width, height );

	S_Log( "Frame buffer address       : %p", client->frameBuffer );

	client->format.bitsPerPixel = 32;
	client->format.redShift = 0;
	client->format.greenShift = 8;
	client->format.blueShift = 16;
	client->format.redMax = 255;
	client->format.greenMax = 255;
	client->format.blueMax = 255;

	SetFormatAndEncodings( client );

	g_pluginInterface.formatTexture( vnc->id, SxTextureFormat_R8G8B8X8_SRGB );
	g_pluginInterface.sizeTexture( vnc->id, width, height );

	vnc->width = width;
	vnc->height = height;

	VNCThread_RebuildGeometry( vnc );

	Prof_Stop( PROF_VNC_THREAD_HANDLE_RESIZE );

	return TRUE;
}


#if USE_OVERLAY

void VNCThread_SetAlpha( SVNCWidget *vnc, int x, int y, int w, int h )
{
	rfbClient 	*client;
	uint 		xi;
	uint 		yi;
	uint 		stride;
	byte 		*data;

	client = vnc->client;
	assert( client );

	assert( client->frameBuffer );
	data = client->frameBuffer + (y * client->width + x) * 4;

	stride = (client->width - w) * 4;

	// Fill background alpha with 1s.
	for ( yi = 0; yi < h; yi++ )
	{
		for ( xi = 0; xi < w; xi++ )
		{
			data[3] = 0xff;
			data += 4;
		}
		data += stride;
	}

	// Fill border edge alpha with 0s.
	if ( x == 0 )
	{
		data = client->frameBuffer + (y * client->width + x) * 4;
		stride = client->width * 4;

		for ( yi = 0; yi < h; yi++ )
		{
			data[3] = 0;
			data += stride;
		}
	}

	if ( x + w == client->width )
	{
		data = client->frameBuffer + (y * client->width + x + w - 1) * 4;
		stride = client->width * 4;

		for ( yi = 0; yi < h; yi++ )
		{
			data[3] = 0;
			data += stride;
		}
	}

	if ( y == 0 )
	{
		data = client->frameBuffer + x * 4;

		for ( xi = 0; xi < w; xi++ )
		{
			data[3] = 0;
			data += 4;
		}
	}

	if ( y + h == client->height )
	{
		data = client->frameBuffer + ((y + h - 1) * client->width + x) * 4;

		for ( xi = 0; xi < w; xi++ )
		{
			data[3] = 0;
			data += 4;
		}
	}
}

#endif // #if USE_OVERLAY


void VNCThread_UpdateTextureRect( SVNCWidget *vnc, int x, int y, int width, int height )
{
	rfbClient 		*client;
	byte 	 		*buffer;
	int 			yc;
	byte 			*frameBuffer;
	uint 			frameBufferWidth;

	Prof_Start( PROF_VNC_THREAD_UPDATE_TEXTURE_RECT );

	client = vnc->client;
	assert( client );

	if ( x < 0 )
	{
		width += x;
		x = 0;
	}
	if ( y < 0 )
	{
		height += y;
		y = 0;
	}

	if ( x + width > client->width )
		width = client->width - x;
	if ( y + height > client->height )
		height = client->height - y;

	if ( width == 0 || height == 0 )
	{
		Prof_Stop( PROF_VNC_THREAD_UPDATE_TEXTURE_RECT );
		return;
	}

	frameBuffer = client->frameBuffer;
	frameBufferWidth = client->width;

	buffer = (byte *)malloc( width * height * 4 );
	if ( !buffer )
	{
		S_Log( "VNCThread_UpdateTextureRect: Failed to allocated %d bytes; skipping update.", width * height * 4 );
		Prof_Stop( PROF_VNC_THREAD_UPDATE_TEXTURE_RECT );
		return;
	}

	for ( yc = 0; yc < height; yc++ )
		memcpy( &buffer[(yc * width) * 4], &frameBuffer[((y + yc) * frameBufferWidth + x) * 4], width * 4 );

	g_pluginInterface.updateTextureRect( vnc->id, x, y, width, height, width * 4, buffer );

	free( buffer );

	vnc->updatePending = strue;

	Prof_Stop( PROF_VNC_THREAD_UPDATE_TEXTURE_RECT );
}


void vnc_thread_update( rfbClient *client, int x, int y, int w, int h )
{
	SVNCWidget 		*vnc;

	Prof_Start( PROF_VNC_THREAD_HANDLE_UPDATE );

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

#if USE_OVERLAY
	VNCThread_SetAlpha( vnc, x, y, w, h );
#endif // #if USE_OVERLAY

	if ( w == 1 && h == 1 )
		S_Log( "Got weird 1x1 rectangle at %d,%d", x, y );

	VNCThread_UpdateTextureRect( vnc, x, y, w, h );

#if STRESS_RESIZE
	static int delay = 0;
	if ( ++delay == 1000 )
	{
		vnc_thread_resize( client );
		delay = 0;
	}
#endif // #if STRESS_RESIZE

	Prof_Stop( PROF_VNC_THREAD_HANDLE_UPDATE );
}


void vnc_thread_finished_updates( rfbClient *client )
{
	SVNCWidget 		*vnc;

	Prof_Start( PROF_VNC_THREAD_HANDLE_FINISHED_UPDATES );

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	if ( vnc->updatePending )
	{
		g_pluginInterface.presentTexture( vnc->id );
		vnc->updatePending = sfalse;
	}

	Prof_Stop( PROF_VNC_THREAD_HANDLE_FINISHED_UPDATES );
}


void vnc_thread_got_cursor_shape( rfbClient *client, int xhot, int yhot, int width, int height, int bytesPerPixel )
{
	SVNCWidget 		*vnc;
	byte 			*buffer;
	byte 			maskPixel;
	int 			x;
	int 			y;

	Prof_Start( PROF_VNC_THREAD_HANDLE_CURSOR_SHAPE );

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	vnc->cursor.width = width;
	vnc->cursor.height = height;
	vnc->cursor.xHot = xhot;
	vnc->cursor.yHot = yhot;

	buffer = (byte *)malloc( vnc->cursor.width * vnc->cursor.height * 4 );
	if ( !buffer )
	{
		S_Log( "vnc_thread_got_cursor_shape: Unable to allocate %d bytes for cursor data.", vnc->cursor.width * vnc->cursor.height * 4 );
		return;
	}

    memcpy( buffer, client->rcSource, width * height * 4 );

    for ( y = 0; y < height; y++ )
    {
        for ( x = 0; x < width; x++ )
        {
            maskPixel = client->rcMask[y * width + x];
            buffer[(y * width + x) * 4 + 3] = maskPixel ? 0xff : 0x00;
		}
	}

	g_pluginInterface.sizeTexture( vnc->cursorId, width, height );
	g_pluginInterface.updateTextureRect( vnc->cursorId, 0, 0, width, height, width * 4, buffer );
	g_pluginInterface.presentTexture( vnc->cursorId );

	free( buffer );

	Prof_Stop( PROF_VNC_THREAD_HANDLE_CURSOR_SHAPE );
}


static rfbBool vnc_thread_handle_cursor_pos( rfbClient *client, int x, int y )
{
	SVNCWidget 	*vnc;

	Prof_Start( PROF_VNC_THREAD_HANDLE_CURSOR_POS );

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	VNC_SetCursorPos( vnc, x, y );

	Prof_Stop( PROF_VNC_THREAD_HANDLE_CURSOR_POS );

	return TRUE;
}


static void vnc_thread_got_x_cut_text( rfbClient *client, const char *text, int textlen )
{
	SVNCWidget 		*vnc;

	assert( client );

	vnc = (SVNCWidget *)rfbClientGetClientData( client, &s_vncGlob );
	assert( vnc );

	while ( isspace( *text ) )
		text++;
	
	if ( strncmp( text, "$$!", 3 ) == 0 )
	{
		g_pluginInterface.postMessage( text + 3 );
	}	
	else
	{
		S_Log( "Clipboard text: %s", text );
	}
}


static sbool VNCThread_Connect( SVNCWidget *vnc )
{
	rfbClient 	*client;
	int 		argc;
	const char 	*argv[2];

	assert( vnc );
	assert( !vnc->client );

	S_Log( "VNCThread_Connect: Connecting to %s...", vnc->server );

	client = rfbGetClient( 8, 3, 4 );
	if ( !client )
		S_Fail( "Failed to create VNC client." );

	vnc->client = client;

	client->GetPassword = vnc_thread_get_password;

	client->canHandleNewFBSize = TRUE;
	client->MallocFrameBuffer = vnc_thread_resize;

	client->GotFrameBufferUpdate = vnc_thread_update;
	client->FinishedFrameBufferUpdate = vnc_thread_finished_updates;
	// $$$ Can we implement this to use accelerated blits?
	// client->GotCopyRect = vnc_thread_copy_rect

	client->appData.useRemoteCursor = TRUE;
	client->GotCursorShape = vnc_thread_got_cursor_shape;
	client->HandleCursorPos = vnc_thread_handle_cursor_pos;
	client->GotXCutText = vnc_thread_got_x_cut_text;

	// Expedited Forwarding (EF) PHB
	// Note that Wikipedia gives the value as 46, but the libvncserver release notes give 184, which is 46 << 2.
	client->QoS_DSCP = 184;

	rfbClientSetClientData( client, &s_vncGlob, vnc );

	argc = 2;
	argv[0] = "Shellspace";
	argv[1] = vnc->server;

	if ( !rfbInitClient( client, &argc, const_cast< char ** >( argv ) ) ) 
	{
		vnc->client = NULL; // rfbInitClient frees the client on failure to connect.
		return sfalse;
	}

	S_Log( "VNCThread_Connect: Connected to %s.", vnc->server );

	vnc->state = VNCSTATE_CONNECTED;

	return strue;
}


static sbool VNCThread_Input( SVNCWidget *vnc )
{
	int 		timeout;
	rfbClient 	*client;
	int 		result;

	Prof_Start( PROF_VNC_THREAD_INPUT );

	client = vnc->client;
	assert( client );

	timeout = 10 * 1000; // 10 milliseconds

	Prof_Start( PROF_VNC_THREAD_WAIT );
	result = WaitForMessage( client, timeout );
	Prof_Stop( PROF_VNC_THREAD_WAIT );
	if ( result < 0 )
	{
		S_Log( "VNC WaitForMessage failed: %i", result );
		Prof_Stop( PROF_VNC_THREAD_INPUT );
		return sfalse;
	}

	if ( result )
	{
		Prof_Start( PROF_VNC_THREAD_HANDLE );
		result = HandleRFBServerMessage( client );
		Prof_Stop( PROF_VNC_THREAD_HANDLE );
		if ( !result )
		{
			S_Log( "HandleRFBServerMessage failed." );
			Prof_Stop( PROF_VNC_THREAD_INPUT );
			return sfalse;
		}
	}

	Prof_Stop( PROF_VNC_THREAD_INPUT );
	return strue;
}


SVNCKeyMap s_vncKeyMap[] =
{
	{ "unknown"   , AKEYCODE_UNKNOWN        , XK_Escape      },
	{ "bkspc"     , AKEYCODE_DEL            , XK_BackSpace   },
	{ "tab"       , AKEYCODE_TAB            , XK_Tab         },
	{ "clear"     , AKEYCODE_UNKNOWN        , XK_Clear       },
	{ "return"    , AKEYCODE_ENTER          , XK_Return      },
	{ "escape"    , AKEYCODE_ESCAPE         , XK_Escape      },
	{ "space"     , AKEYCODE_SPACE          , XK_space       },
	{ "delete"    , AKEYCODE_FORWARD_DEL    , XK_Delete      },
	{ "kp0"       , AKEYCODE_NUMPAD_0       , XK_KP_0        },
	{ "kp1"       , AKEYCODE_NUMPAD_1       , XK_KP_1        },
	{ "kp2"       , AKEYCODE_NUMPAD_2       , XK_KP_2        },
	{ "kp3"       , AKEYCODE_NUMPAD_3       , XK_KP_3        },
	{ "kp4"       , AKEYCODE_NUMPAD_4       , XK_KP_4        },
	{ "kp5"       , AKEYCODE_NUMPAD_5       , XK_KP_5        },
	{ "kp6"       , AKEYCODE_NUMPAD_6       , XK_KP_6        },
	{ "kp7"       , AKEYCODE_NUMPAD_7       , XK_KP_7        },
	{ "kp8"       , AKEYCODE_NUMPAD_8       , XK_KP_8        },
	{ "kp9"       , AKEYCODE_NUMPAD_9       , XK_KP_9        },
	{ "kpdot"     , AKEYCODE_NUMPAD_DOT     , XK_KP_Decimal  },
	{ "kpdiv"     , AKEYCODE_NUMPAD_DIVIDE  , XK_KP_Divide   },
	{ "kpmul"     , AKEYCODE_NUMPAD_MULTIPLY, XK_KP_Multiply },
	{ "kpsub"     , AKEYCODE_NUMPAD_SUBTRACT, XK_KP_Subtract },
	{ "kpadd"     , AKEYCODE_NUMPAD_ADD     , XK_KP_Add      },
	{ "kpenter"   , AKEYCODE_NUMPAD_ENTER   , XK_KP_Enter    },
	{ "kpequal"   , AKEYCODE_NUMPAD_EQUALS  , XK_KP_Equal    },
	{ "up"        , AKEYCODE_DPAD_UP        , XK_Up          },
	{ "down"      , AKEYCODE_DPAD_DOWN      , XK_Down        },
	{ "left"      , AKEYCODE_DPAD_LEFT      , XK_Left        },
	{ "right"     , AKEYCODE_DPAD_RIGHT     , XK_Right       },
	{ "insert"    , AKEYCODE_INSERT         , XK_Insert      },
	{ "home"      , AKEYCODE_MOVE_HOME      , XK_Home        },
	{ "end"       , AKEYCODE_MOVE_END       , XK_End         },
	{ "pgup"      , AKEYCODE_PAGE_UP        , XK_Page_Up     },
	{ "pgdn"      , AKEYCODE_PAGE_DOWN      , XK_Page_Down   },
	{ "f1"        , AKEYCODE_F1             , XK_F1          },
	{ "f2"        , AKEYCODE_F2             , XK_F2          },
	{ "f3"        , AKEYCODE_F3             , XK_F3          },
	{ "f4"        , AKEYCODE_F4             , XK_F4          },
	{ "f5"        , AKEYCODE_F5             , XK_F5          },
	{ "f6"        , AKEYCODE_F6             , XK_F6          },
	{ "f7"        , AKEYCODE_F7             , XK_F7          },
	{ "f8"        , AKEYCODE_F8             , XK_F8          },
	{ "f9"        , AKEYCODE_F9             , XK_F9          },
	{ "f10"       , AKEYCODE_F10            , XK_F10         },
	{ "f11"       , AKEYCODE_F11            , XK_F11         },
	{ "f12"       , AKEYCODE_F12            , XK_F12         },
	{ "f13"       , AKEYCODE_UNKNOWN        , XK_F13         },
	{ "f14"       , AKEYCODE_UNKNOWN        , XK_F14         },
	{ "f15"       , AKEYCODE_UNKNOWN        , XK_F15         },
	{ "numlock"   , AKEYCODE_NUM_LOCK       , XK_Num_Lock    },
	{ "capslock"  , AKEYCODE_CAPS_LOCK      , XK_Caps_Lock   },
	{ "scrolllock", AKEYCODE_SCROLL_LOCK    , XK_Scroll_Lock },
	{ "rshift"    , AKEYCODE_SHIFT_RIGHT    , XK_Shift_R     },
	{ "lshift"    , AKEYCODE_SHIFT_LEFT     , XK_Shift_L     },
	{ "rctrl"     , AKEYCODE_CTRL_RIGHT     , XK_Control_R   },
	{ "lctrl"     , AKEYCODE_CTRL_LEFT      , XK_Control_L   },
	{ "ralt"      , AKEYCODE_ALT_RIGHT      , XK_Alt_R       },
	{ "lalt"      , AKEYCODE_ALT_LEFT       , XK_Alt_L       },
	{ "rmeta"     , AKEYCODE_UNKNOWN        , XK_Meta_R      },
	{ "lmeta"     , AKEYCODE_UNKNOWN        , XK_Meta_L      },
	{ "rsuper"    , AKEYCODE_UNKNOWN        , XK_Super_R     },
	{ "lsuper"    , AKEYCODE_UNKNOWN        , XK_Super_L     },
	{ "mode"      , AKEYCODE_UNKNOWN        , XK_Mode_switch },
	{ "help"      , AKEYCODE_UNKNOWN        , XK_Help        },
	{ "print"     , AKEYCODE_BREAK          , XK_Print       },
	{ "sysreq"    , AKEYCODE_SYSRQ          , XK_Sys_Req     },
	{ "break"     , AKEYCODE_UNKNOWN        , XK_Break       },
	{ "0"         , AKEYCODE_0              , '0'            },
	{ "1"         , AKEYCODE_1              , '1'            },
	{ "2"         , AKEYCODE_2              , '2'            },
	{ "3"         , AKEYCODE_3              , '3'            },
	{ "4"         , AKEYCODE_4              , '4'            },
	{ "5"         , AKEYCODE_5              , '5'            },
	{ "6"         , AKEYCODE_6              , '6'            },
	{ "7"         , AKEYCODE_7              , '7'            },
	{ "8"         , AKEYCODE_8              , '8'            },
	{ "9"         , AKEYCODE_9              , '9'            },
	{ "a"         , AKEYCODE_A              , 'a'            },
	{ "b"         , AKEYCODE_B              , 'b'            },
	{ "c"         , AKEYCODE_C              , 'c'            },
	{ "d"         , AKEYCODE_D              , 'd'            },
	{ "e"         , AKEYCODE_E              , 'e'            },
	{ "f"         , AKEYCODE_F              , 'f'            },
	{ "g"         , AKEYCODE_G              , 'g'            },
	{ "h"         , AKEYCODE_H              , 'h'            },
	{ "i"         , AKEYCODE_I              , 'i'            },
	{ "j"         , AKEYCODE_J              , 'j'            },
	{ "k"         , AKEYCODE_K              , 'k'            },
	{ "l"         , AKEYCODE_L              , 'l'            },
	{ "m"         , AKEYCODE_M              , 'm'            },
	{ "n"         , AKEYCODE_N              , 'n'            },
	{ "o"         , AKEYCODE_O              , 'o'            },
	{ "p"         , AKEYCODE_P              , 'p'            },
	{ "q"         , AKEYCODE_Q              , 'q'            },
	{ "r"         , AKEYCODE_R              , 'r'            },
	{ "s"         , AKEYCODE_S              , 's'            },
	{ "t"         , AKEYCODE_T              , 't'            },
	{ "u"         , AKEYCODE_U              , 'u'            },
	{ "v"         , AKEYCODE_V              , 'v'            },
	{ "w"         , AKEYCODE_W              , 'w'            },
	{ "x"         , AKEYCODE_X              , 'x'            },
	{ "y"         , AKEYCODE_Y              , 'y'            },
	{ "z"         , AKEYCODE_Z              , 'z'            },
	{ "grave"     , AKEYCODE_GRAVE          , '~'            },
	{ "minus"     , AKEYCODE_MINUS          , '-'            },
	{ "plus"      , AKEYCODE_PLUS           , '+'            },
	{ "lbracket"  , AKEYCODE_LEFT_BRACKET   , '['            },
	{ "rbracket"  , AKEYCODE_RIGHT_BRACKET  , ']'            },
	{ "backslash" , AKEYCODE_BACKSLASH      , '\\'           },
	{ "semicolon" , AKEYCODE_SEMICOLON      , ';'            },
	{ "apostrophe", AKEYCODE_APOSTROPHE     , '\''           },
	{ "slash"     , AKEYCODE_SLASH          , '/'            },
	{ "comma"     , AKEYCODE_COMMA          , ','            },
	{ "period"    , AKEYCODE_PERIOD         , '.'            },
};


uint VNC_KeyCodeForAndroidCode( uint androidCode )
{
	SVNCKeyMap 	*map;

	map = s_vncKeyMap;

	while ( map->name )
	{
		if ( map->androidCode == androidCode )
			return map->vncCode;

		map++;
	}

	return INVALID_KEY_CODE;
}


void VNC_KeyCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;
	int 		code;
	int 		vncCode;
	sbool 		down;

	vnc = (SVNCWidget *)context;
	assert( vnc );

	code = atoi( Msg_Argv( msg, 1 ) );
	down = S_streq( Msg_Argv( msg, 2 ), "down" );

	S_Log( "VNC_KeyCmd: %d %d", code, down );

	vncCode = VNC_KeyCodeForAndroidCode( code );
	if ( vncCode != INVALID_KEY_CODE )
		SendKeyEvent( vnc->client, vncCode, down );
}


void VNC_MouseCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;
	float 		x;
	float 		y;
	int 		xFrame;
	int 		yFrame;
	uint 		buttons;

	vnc = (SVNCWidget *)context;
	assert( vnc );

	x = atof( Msg_Argv( msg, 1 ) );
	y = atof( Msg_Argv( msg, 2 ) );

	xFrame = round( (x * 0.5f + 0.5f) * vnc->width );
	yFrame = round( (-y * 0.5f + 0.5f) * vnc->height );

	buttons = 0;

	if ( atoi( Msg_Argv( msg, 3 ) ) )
		buttons |= rfbButton1Mask;
	if ( atoi( Msg_Argv( msg, 4 ) ) )
		buttons |= rfbButton2Mask;
	if ( atoi( Msg_Argv( msg, 5 ) ) )
		buttons |= rfbButton3Mask;

	if ( xFrame != (int)vnc->cursor.xPos || yFrame != (int)vnc->cursor.yPos || buttons != vnc->cursor.buttons )
	{
		VNC_SetCursorPos( vnc, xFrame, yFrame );

		if ( vnc->cursor.sendUpdates || buttons || buttons != vnc->cursor.buttons )
			SendPointerEvent( vnc->client, xFrame, yFrame, buttons );

		vnc->cursor.buttons = buttons;
	}
}


void VNC_ArcCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;

	vnc = (SVNCWidget *)context;
	assert( vnc );

	vnc->latArc = atof( Msg_Argv( msg, 1 ) );
	vnc->lonArc = atof( Msg_Argv( msg, 2 ) );
	vnc->depth = atof( Msg_Argv( msg, 3 ) );

	VNCThread_RebuildGeometry( vnc );
}


// void VNC_ZPushCmd( const SMsg *msg, void *context )
// {
// 	SVNCWidget 	*vnc;

// 	vnc = (SVNCWidget *)context;
// 	assert( vnc );

// 	if ( !Msg_SetFloatCmd( msg, &vnc->globeZPush, -100.0f, 100.0f ) )
// 		return;

// 	VNCThread_RebuildGeometry( vnc );
// }


void VNC_SendCursorCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;

	vnc = (SVNCWidget *)context;
	assert( vnc );

	if ( !Msg_SetBoolCmd( msg, &vnc->cursor.sendUpdates ) )
		return;
}


SMsgCmd s_vncWidgetCmds[] =
{
	{ "key", 			VNC_KeyCmd, 			"key <code> <down>" },
	{ "mouse", 			VNC_MouseCmd, 			"mouse <x> <y> <buttons>" },
	{ "arc",          	VNC_ArcCmd,             "arc <value>" },
	{ "sendcursor",   	VNC_SendCursorCmd,      "sendcursor <true|false>" },
	// { "zpush",          VNC_ZPushCmd,           "zpush <value>" },
	{ NULL, NULL, NULL }
};


void VNCThread_Messages( SVNCWidget *vnc )
{
	char 	*text;
	SMsg 	msg;

	assert( vnc );

	text = MsgQueue_Get( &vnc->msgQueue, 1 );
	if ( !text )
		return;
	
	Msg_ParseString( &msg, text );

	free( text );

	if ( Msg_Empty( &msg ) )
		return;

	if ( Msg_IsArgv( &msg, 0, "vnc" ) )
		Msg_Shift( &msg, 1 );

	if ( Msg_IsArgv( &msg, 0, vnc->id ) )
		Msg_Shift( &msg, 1 );

	MsgCmd_Dispatch( &msg, s_vncWidgetCmds, vnc );
}


static void VNCThread_Loop( SVNCWidget *vnc )
{
	assert( vnc );
	assert( vnc->client );

	while ( !vnc->disconnect )
	{	
		Prof_Start( PROF_VNC_THREAD );

		if ( !VNCThread_Input( vnc ) )
			break;

		VNCThread_Messages( vnc );

		Prof_Stop( PROF_VNC_THREAD );
	}
}


static void VNCThread_Cleanup( SVNCWidget *vnc )
{
	assert( vnc );

	S_Log( "VNCThread_Cleanup: Disconnected from %s; cleaning up.", vnc->server );

	if ( vnc->client )
	{
		rfbClientCleanup( vnc->client );
		vnc->client = NULL;
	}

	free( vnc->server );
	vnc->server = NULL;

	free( vnc->password );
	vnc->password = NULL;

	vnc->state = VNCSTATE_DISCONNECTED;
}


static void *VNCThread( void *context )
{
	SVNCWidget	*vnc;

	pthread_setname_np( pthread_self(), "VNC" );

	vnc = (SVNCWidget *)context;
	assert( vnc );

	if ( VNCThread_Connect( vnc ) )
	{
		VNCThread_Loop( vnc );
	}

	VNCThread_Cleanup( vnc );

	return 0;
}


void VNC_Connect( SVNCWidget *vnc, const char *server, const char *password )
{
	int 	err;

	assert( vnc );

	if ( vnc->state != VNCSTATE_DISCONNECTED )
	{
		S_Log( "VNC_Connect: Already connected to %s.", vnc->server );
		return;
	}

	assert( vnc );
	assert( !vnc->server );
	assert( !vnc->password );

	vnc->server = strdup( server );
	vnc->password = strdup( password );

	vnc->state = VNCSTATE_CONNECTING;

	err = pthread_create( &vnc->thread, NULL, VNCThread, vnc );
	if ( err != 0 )
		S_Fail( "VNC_Connect: pthread_create returned %i", err );
}


void VNC_Disconnect( SVNCWidget *vnc )
{
	assert( vnc );

	if ( vnc->state == VNCSTATE_DISCONNECTED )
	{
		S_Log( "VNC_Disconnect: Not connected." );
		return;
	}

	vnc->disconnect = strue;

	while ( vnc->state != VNCSTATE_DISCONNECTED )
		Thread_Sleep( 3 );

	S_Log( "VNC_Disconnect: Finished disconnecting." );

	vnc->disconnect = sfalse;
}


#if 0
#define SETUP_CLIPBOARD(error) \
    struct LocalReferenceHolder refs = LocalReferenceHolder_Setup(__FUNCTION__); \
    JNIEnv* env = Android_JNI_GetEnv(); \
    if (!LocalReferenceHolder_Init(&refs, env)) { \
        LocalReferenceHolder_Cleanup(&refs); \
        return error; \
    } \
    jobject clipboard = Android_JNI_GetSystemServiceObject("clipboard"); \
    if (!clipboard) { \
        LocalReferenceHolder_Cleanup(&refs); \
        return error; \
    }

#define CLEANUP_CLIPBOARD() \
    LocalReferenceHolder_Cleanup(&refs);

int Android_JNI_SetClipboardText(const char* text)
{
    SETUP_CLIPBOARD(-1)

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "setText", "(Ljava/lang/CharSequence;)V");
    jstring string = (*env)->NewStringUTF(env, text);
    (*env)->CallVoidMethod(env, clipboard, mid, string);
    (*env)->DeleteGlobalRef(env, clipboard);
    (*env)->DeleteLocalRef(env, string);

    CLEANUP_CLIPBOARD();

    return 0;
}

char* Android_JNI_GetClipboardText()
{
    SETUP_CLIPBOARD(SDL_strdup(""))

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "getText", "()Ljava/lang/CharSequence;");
    jobject sequence = (*env)->CallObjectMethod(env, clipboard, mid);
    (*env)->DeleteGlobalRef(env, clipboard);
    if (sequence) {
        mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, sequence), "toString", "()Ljava/lang/String;");
        jstring string = (jstring)((*env)->CallObjectMethod(env, sequence, mid));
        const char* utf = (*env)->GetStringUTFChars(env, string, 0);
        if (utf) {
            char* text = SDL_strdup(utf);
            (*env)->ReleaseStringUTFChars(env, string, utf);

            CLEANUP_CLIPBOARD();

            return text;
        }
    }

    CLEANUP_CLIPBOARD();    

    return SDL_strdup("");
}

SDL_bool Android_JNI_HasClipboardText()
{
    SETUP_CLIPBOARD(SDL_FALSE)

    jmethodID mid = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, clipboard), "hasText", "()Z");
    jboolean has = (*env)->CallBooleanMethod(env, clipboard, mid);
    (*env)->DeleteGlobalRef(env, clipboard);

    CLEANUP_CLIPBOARD();
    
    return has ? SDL_TRUE : SDL_FALSE;
}
#endif


// sbool VNC_Command( SVNCWidget *vnc )
// {
// 	const char 	*cmd;
// 	int 		vncCode;
// 	SVNCKeyMap 	*map;

// 	cmd = Cmd_Argv( 0 );

// 	if ( cmd[0] == '+' || cmd[0] == '-' )
// 	{
// 		vncCode = INVALID_KEY_CODE;

// 		if ( cmd[1] >= 'a' && cmd[1] <= 'z' && cmd[2] == 0 )
// 		{
// 			vncCode = cmd[1];
// 		}
// 		else if ( cmd[1] >= '0' && cmd[1] <= '9' && cmd[2] == 0 )
// 		{
// 			vncCode = cmd[1];
// 		}
// 		else
// 		{
// 			map = s_vncKeyMap;

// 			while ( map->name )
// 			{
// 				if ( strcasecmp( map->name, cmd + 1 ) == 0 )
// 				{
// 					vncCode = map->vncCode;
// 					break;
// 				}

// 				map++;
// 			}
// 		}

// 		if ( vncCode != INVALID_KEY_CODE )
// 		{
// 			VNC_KeyboardEvent( vnc, vncCode, cmd[0] == '+' );
// 			return strue;
// 		}
// 	}

// 	if ( strcasecmp( cmd, "headmouse" ) == 0 )
// 	{
// 		if ( Cmd_Argc() != 2 )
// 		{
// 			S_Log( "Usage: headmouse <on/off/toggle>" );
// 			return strue;
// 		}

// 		if ( strcasecmp( Cmd_Argv( 1 ), "on" ) == 0 )
// 		{
// 			s_vncGlob.headmouse = strue;
// 		}
// 		else if ( strcasecmp( Cmd_Argv( 1 ), "off" ) == 0 )
// 		{	
// 			s_vncGlob.headmouse = sfalse;
// 		}
// 		else if ( strcasecmp( Cmd_Argv( 1 ), "toggle" ) == 0 )
// 		{
// 			s_vncGlob.headmouse = !s_vncGlob.headmouse;
// 		}
// 		else
// 		{
// 			S_Log( "Usage: headmouse <on/off/toggle>" );
// 		}

// 		return strue;
// 	}


// 	return sfalse;
// }

SVNCWidget *VNC_AllocWidget( SxWidgetHandle id )
{
	uint 		wIter;
	SVNCWidget 	*vnc;

	for ( wIter = 0; wIter < VNC_WIDGET_LIMIT; wIter++ )
	{
		vnc = &s_vncGlob.widgetPool[wIter];
		if ( !vnc->id )
		{
			memset( vnc, 0, sizeof( SVNCWidget ) );
			vnc->id = strdup( id );
			return vnc;
		};
	}

	S_Log( "VNC_AllocWidget: Cannot allocate %s; limit of %i widgets reached.", id, VNC_WIDGET_LIMIT );

	return NULL;
}


void VNC_FreeWidget( SVNCWidget *vnc )
{
	assert( vnc->id );

	free( (char *)vnc->id );
	vnc->id = NULL;
}


sbool VNC_WidgetExists( SxWidgetHandle id )
{
	uint 		wIter;
	SVNCWidget 	*widget;

	for ( wIter = 0; wIter < VNC_WIDGET_LIMIT; wIter++ )
	{
		widget = &s_vncGlob.widgetPool[wIter];
		if ( widget->id && S_streq( widget->id, id ) )
			return strue;
	}

	return sfalse;
}


SVNCWidget *VNC_GetWidget( SxWidgetHandle id )
{
	uint 		wIter;
	SVNCWidget 	*widget;

	for ( wIter = 0; wIter < VNC_WIDGET_LIMIT; wIter++ )
	{
		widget = &s_vncGlob.widgetPool[wIter];
		if ( widget->id && S_strcmp( widget->id, id ) == 0 )
			return widget;
	}

	S_Log( "VNC_GetWidget: Widget %s does not exist.", id );

	return NULL;
}


void VNC_WidgetCmd( const SMsg *msg )
{
	SVNCWidget 		*widget;
	SxWidgetHandle 	wid;
	char 			msgBuf[MSG_LIMIT];

	wid = Msg_Argv( msg, 0 );

	Msg_Format( msg, msgBuf, MSG_LIMIT );

	widget = VNC_GetWidget( wid );
	if ( !widget )
	{
		S_Log( "VNC_WidgetCmd: This command was not recognized as either a plugin command or a valid widget id: %s", msgBuf );
		return;
	}

	MsgQueue_Put( &widget->msgQueue, msgBuf );
}


void VNC_CreateCmd( const SMsg *msg, void *context )
{
	SxWidgetHandle 	id;
	SVNCWidget 		*vnc;
	char 			msgBuf[MSG_LIMIT];

	id = Msg_Argv( msg, 1 );
	
	if ( VNC_WidgetExists( id ) )
	{
		S_Log( "VNC_CreateCmd: Widget %s already exists.", id );
		return;
	}

	vnc = VNC_AllocWidget( id );
	if ( !vnc )
		return;

	g_pluginInterface.registerWidget( vnc->id );

	// Primary entity
	g_pluginInterface.registerEntity( vnc->id );

	g_pluginInterface.registerTexture( vnc->id );
	g_pluginInterface.setEntityTexture( vnc->id, vnc->id );

	g_pluginInterface.registerGeometry( vnc->id );
	g_pluginInterface.setEntityGeometry( vnc->id, vnc->id );

	// Cursor entity
	VNCThread_BuildCursor( vnc );

	g_pluginInterface.parentEntity( vnc->cursorId, vnc->id );

	snprintf( msgBuf, MSG_LIMIT, "shell register vnc %s %s", vnc->id, vnc->id );
	g_pluginInterface.postMessage( msgBuf );
}


void VNC_DestroyCmd( const SMsg *msg, void *context )
{
	SxWidgetHandle 	id;
	SVNCWidget 		*vnc;
	char 			msgBuf[MSG_LIMIT];

	id = Msg_Argv( msg, 1 );
	
	vnc = VNC_GetWidget( id );
	if ( !vnc )
	{
		S_Log( "VNC_DestroyCmd: Widget %s does not exist.", id );
		return;
	}

	if ( vnc->state != VNCSTATE_DISCONNECTED )
		VNC_Disconnect( vnc );

	g_pluginInterface.unregisterWidget( vnc->id );

	g_pluginInterface.unregisterEntity( vnc->id );
	g_pluginInterface.unregisterTexture( vnc->id );
	g_pluginInterface.unregisterGeometry( vnc->id );

	g_pluginInterface.unregisterEntity( vnc->cursorId );
	g_pluginInterface.unregisterTexture( vnc->cursorId );
	g_pluginInterface.unregisterGeometry( vnc->cursorId );

	free( (char *)vnc->cursorId );

	snprintf( msgBuf, MSG_LIMIT, "shell unregister %s", vnc->id );
	g_pluginInterface.postMessage( msgBuf );

	VNC_FreeWidget( vnc );
}


void VNC_ConnectCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;
	const char 	*server;
	const char 	*password;

	if ( Msg_Argc( msg ) != 3 && Msg_Argc( msg ) != 4 )
	{
		S_Log( "Usage: connect <wid> <server> [password]" );
		return;
	}

	vnc = VNC_GetWidget( Msg_Argv( msg, 1 ) );
	if ( !vnc )
		return;

	server = Msg_Argv( msg, 2 );
	password = Msg_Argv( msg, 3 );

	VNC_Connect( vnc, server, password );
}


void VNC_DisconnectCmd( const SMsg *msg, void *context )
{
	SVNCWidget 	*vnc;

	vnc = VNC_GetWidget( Msg_Argv( msg, 1 ) );
	if ( !vnc )
		return;

	VNC_Disconnect( vnc );
}


// $$$ Need some way to register a list of commands w/documentation.
//     Other plugins need to be able to interrogate the list of commands,
//     e.g. for console autocomplete and context menu population.
// 	   Really just need to formalize the help string syntax.
//     Could register a "help" command that takes the table and returns all the strings
//      by sending them to a given widget (like a menu widget).
SMsgCmd s_vncCmds[] =
{
	{ "create", 		VNC_CreateCmd, 			"create <wid>" },
	{ "destroy", 		VNC_DestroyCmd, 		"destroy <wid>" },
	{ "connect", 		VNC_ConnectCmd, 		"connect <wid> <server> <port>" },
	{ "disconnect", 	VNC_DisconnectCmd, 		"disconnect <wid>" },
	{ NULL, NULL, NULL }
};


void *VNC_PluginThread( void *context )
{
	char 	msgBuf[MSG_LIMIT];
	SMsg 	msg;

	pthread_setname_np( pthread_self(), "VNC" );

	g_pluginInterface.registerPlugin( "vnc", SxPluginKind_Widget );

	for ( ;; )
	{
		g_pluginInterface.receiveMessage( "vnc", SX_WAIT_INFINITE, msgBuf, MSG_LIMIT );

		Msg_ParseString( &msg, msgBuf );
		if ( Msg_Empty( &msg ) )
			continue;

		if ( Msg_IsArgv( &msg, 0, "vnc" ) )
			Msg_Shift( &msg, 1 );

		if ( Msg_IsArgv( &msg, 0, "unload" ) )
			break;

		if ( !MsgCmd_Dispatch( &msg, s_vncCmds, NULL ) )
			VNC_WidgetCmd( &msg );
	}

	g_pluginInterface.unregisterPlugin( "vnc" );

	return 0;
}


void VNC_InitPlugin()
{
	int err;

	rfbClientLog = rfb_log;
	rfbClientErr = rfb_error;

	s_vncGlob.headmouse = sfalse;

	err = pthread_create( &s_vncGlob.pluginThread, NULL, VNC_PluginThread, NULL );
	if ( err != 0 )
		S_Fail( "VNC_InitPlugin: pthread_create returned %i", err );
}
