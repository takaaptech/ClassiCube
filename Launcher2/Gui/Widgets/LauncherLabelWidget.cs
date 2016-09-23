﻿// ClassicalSharp copyright 2014-2016 UnknownShadow200 | Licensed under MIT
using System;
using System.Drawing;
using ClassicalSharp;

namespace Launcher.Gui.Widgets {

	/// <summary> Represents text that cannot be modified by the user. </summary>
	public sealed class LauncherLabelWidget : LauncherWidget {
		
		Font font;
		public LauncherLabelWidget( LauncherWindow window, string text ) : base( window ) {
			Text = text;
		}
		
		public void SetDrawData( IDrawer2D drawer, string text, Font font,
		                        Anchor horAnchor, Anchor verAnchor, int x, int y ) {
			UpdateLocation( horAnchor, verAnchor, x, y );
			this.font = font;
			SetDrawData( drawer, text );
		}
		
		public void SetDrawData( IDrawer2D drawer, string text) {
			DrawTextArgs args = new DrawTextArgs( text, font, true );
			Size size = drawer.MeasureSize( ref args );
			Width = size.Width; Height = size.Height;
			
			CalculatePosition();
			Text = text;
		}
		
		public override void Redraw( IDrawer2D drawer ) {
			if( Window.Minimised ) return;
			DrawTextArgs args = new DrawTextArgs( Text, font, true );
			drawer.DrawText( ref args, X, Y );
		}
	}
}
