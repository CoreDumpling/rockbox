/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2010 Thomas Martitz
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

package org.rockbox;

import java.nio.ByteBuffer;
import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Handler;
import android.util.Log;
import android.view.MotionEvent;
import android.view.View;

public class RockboxFramebuffer extends View 
{
	private Bitmap btm;
    private ByteBuffer native_buf;


	public RockboxFramebuffer(Context c)
	{
		super(c);
		btm = null;
	}

	public void onDraw(Canvas c) 
	{
		if (btm != null)
			c.drawBitmap(btm, 0.0f, 0.0f, null);
	}
	
	public void java_lcd_init(int lcd_width, int lcd_height, ByteBuffer native_fb)
	{
		btm = Bitmap.createBitmap(lcd_width, lcd_height, Bitmap.Config.RGB_565);
		native_buf = native_fb;
	}
	
	public void java_lcd_update()
	{

		btm.copyPixelsFromBuffer(native_buf);
		postInvalidate();
	}
	
	public void java_lcd_update_rect(int x, int y, int w, int h)
	{
		/* can't copy a partial buffer */
		btm.copyPixelsFromBuffer(native_buf);
		postInvalidate(x, y, x+w, y+h);
	}

	private void LOG(CharSequence text)
	{
		Log.d("RockboxBootloader", (String) text);
	}

	public boolean onTouchEvent(MotionEvent me)
	{
		LOG("onTouchEvent");
		switch (me.getAction())
		{
		case MotionEvent.ACTION_CANCEL:
		case MotionEvent.ACTION_UP:
			touchHandler(0);
			break;
		case MotionEvent.ACTION_MOVE:
		case MotionEvent.ACTION_DOWN:
			touchHandler(1);
			break;
		
		}
		pixelHandler((int)me.getX(), (int)me.getY());
		return true;
	}
	
	/* the two below should only be called from the activity thread */
	public void suspend()
	{	/* suspend, Rockbox will not make any lcd updates */
		set_lcd_active(0);
	}
	public void resume()
	{	/* make updates again, the underlying function will 
		 * send an event */
		set_lcd_active(1);
	}

	public native void set_lcd_active(int active);
	public native void pixelHandler(int x, int y);
	public native void touchHandler(int down);
}