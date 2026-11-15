#ifndef GUI_THEME_H
#define GUI_THEME_H

/* Application-local GTK styling. Match the black/white DAB monitor display
 * without changing the desktop theme or other applications. */
static void apply_receiver_theme(void) {
    static const char css[] =
        "#frequency-display { font-family: monospace; font-size: 24px; font-weight: bold; }"
        "#gain-display, #mode-display { font-family: monospace; font-size: 16px; font-weight: bold; }"
        "frame > border { border: 1px solid #777777; border-radius: 3px; }"
        "#receiver-menu { background-color: #151515; }"
        /* Keep the menu appearance and generous hit area on direct actions. */
        "#receiver-menu > button { min-height: 28px; padding: 6px 12px; border: none; border-radius: 0; }"
        "#receiver-menu > button label { background-color: transparent; }"
        "button.spectrum-arrow { min-width: 18px; min-height: 14px; padding: 0; border-radius: 2px; }"
        "* { color: #ffffff; text-shadow: none; -gtk-icon-shadow: none; }"
        "window, dialog, box, grid, frame, notebook, notebook > header,"
        "scrolledwindow, viewport, textview, textview text, label {"
        " background-color: #000000; }"
        "button, entry, spinbutton, combobox, combobox button, menu, menuitem,"
        "popover, treeview, check, radio {"
        " background-color: #080808; background-image: none;"
        " border-color: #666666; box-shadow: none; }"
        "button:hover, menuitem:hover, menuitem:selected, treeview:selected {"
        " background-color: #202020; color: #ffffff; }"
        "button:active, button:checked, check:checked, radio:checked {"
        " background-color: #16434a; border-color: #32dceb; color: #ffffff; }"
        "button:focus, entry:focus, spinbutton:focus, combobox button:focus {"
        " border-color: #32dceb; outline-color: #32dceb; }"
        "*:disabled { color: #999999; border-color: #333333; }"
        "selection { background-color: #16434a; color: #ffffff; }"
        "#received-programs, #received-programs row { background-color: #000000; }"
        "#received-programs row { border-bottom: 1px solid #333333; transition: none; }"
        "#received-programs row:hover { background-color: #202020; }"
        "#received-programs row:selected { background-color: #16434a; }"
        "#received-programs row label { background-color: transparent; }"
        "scrollbar, scrollbar trough { background-color: #080808; }"
        "scrollbar slider { background-color: #666666; min-width: 8px; min-height: 8px; }"
        "tooltip, tooltip label { background-color: #101010; color: #ffffff; }"
        "#decoder-grid { border-top: 1px solid #444444; border-right: 1px solid #444444; }"
        "#decoder-grid label { border-bottom: 1px solid #444444; padding: 2px 5px; }"
        "#decoder-grid .decoder-caption { border-left: 1px solid #444444; }"
        "#decoder-grid .decoder-value { padding-left: 3px; padding-right: 3px; }"
        "#decoder-grid .decoder-unit { padding-left: 2px; }"
        ".about-title { font-size: 24px; font-weight: bold; }"
        ".about-heading { font-size: 16px; font-weight: bold; }"
        ".about-meta { color: #aaaaaa; }"
        "#about-shortcuts { border-top: 1px solid #444444; border-left: 1px solid #444444; }"
        "#about-shortcuts label { padding: 3px 8px; border-bottom: 1px solid #444444; border-right: 1px solid #444444; }"
        "#about-shortcuts .about-table-heading { background-color: #151515; font-weight: bold; }"
        "#about-shortcuts .about-key { font-family: monospace; font-weight: bold; }"
        "#receiver-power { min-width: 166px; padding: 3px 10px; font-weight: bold; }"
        "#iq-record { padding: 3px 8px; font-weight: bold; }"
        "#iq-record label { background-color: transparent; }"
        "#iq-record:checked { background-color: #b52323; border-color: #ff7070; }"
        "#receiver-power box, #receiver-power label { background-color: transparent; color: #ffffff; }"
        "#receiver-power .power-lamp { font-size: 20px; }"
        "#receiver-power.receiver-off { background-color: #252525; border-color: #777777; }"
        "#receiver-power.receiver-off .power-lamp { color: #999999; }"
        "#receiver-power.receiver-running { background-color: #145c2e; border-color: #53ed85; }"
        "#receiver-power.receiver-running .power-lamp { color: #70ff9c; }"
        "#receiver-power.receiver-stopping { background-color: #664b0a; border-color: #ffd151; }"
        "#receiver-power.receiver-stopping .power-lamp { color: #ffdb60; }"
        "#receiver-power.receiver-error { background-color: #6a2020; border-color: #ff7777; }"
        "#receiver-power.receiver-error .power-lamp { color: #ff8888; }"
        "button.information-button { min-height: 18px; padding: 1px 6px; background-color: #252525; border-color: #777777; }"
        "button.information-button label { background-color: transparent; }"
        "button.information-button.information-ready { background-color: #145c2e; border-color: #53ed85; }"
        "button.information-button.information-ready:hover { background-color: #1c743c; }"
        /* Use the same outline for action buttons, toggles and status lamps. */
        "#spectrum-controls > button { border-width: 2px; border-style: solid; border-radius: 2px; }"
        "#spectrum-bw, #spectrum-scatter { min-width: 24px; min-height: 18px; padding: 1px 4px; border-radius: 2px; font-weight: bold; }"
        "#spectrum-bw label, #spectrum-scatter label { background-color: transparent; }"
        "#spectrum-bw:checked, #spectrum-scatter:checked { background-color: #b52323; border-color: #ff7070; color: #ffffff; }"
        "#spectrum-bw:checked:hover, #spectrum-scatter:checked:hover { background-color: #d02b2b; }"
        "#spectrum-usb, #spectrum-fm, #spectrum-fm-data { min-width: 24px; min-height: 18px; padding: 1px 4px; border-radius: 2px; font-weight: bold; border-color: #666666; outline-style: none; }"
        "#spectrum-usb label, #spectrum-fm label, #spectrum-fm-data label { background-color: transparent; }"
        "#spectrum-bonding { min-width: 76px; min-height: 18px; padding: 0 4px; border-radius: 2px; font-size: 9px; font-weight: bold; }"
        "#spectrum-bonding label { background-color: transparent; }"
        "#spectrum-bonding.bonding-lit, #spectrum-bonding.bonding-lit:disabled {"
        " background-color: #145c2e; background-image: none; border-color: #53ed85; opacity: 1; }"
        "#spectrum-bonding.bonding-lit label, #spectrum-bonding.bonding-lit:disabled label {"
        " background-color: transparent; opacity: 1; }"
        /* These actions can be unavailable while their captions stay legible. */
        "#spectrum-bonding, #spectrum-log { opacity: 1; font-weight: bold; }"
        "#spectrum-bonding label, #spectrum-bonding:disabled label,"
        "#spectrum-log label, #spectrum-log:disabled label {"
        " color: #ffffff; font-weight: bold; background-color: transparent; opacity: 1; }"
        "#spectrum-usb:checked, #spectrum-fm:checked, #spectrum-fm-data:checked { background-color: #7a4a08; border-color: #ffcc66; color: #ffffff; }"
        "#spectrum-usb:checked:hover, #spectrum-fm:checked:hover, #spectrum-fm-data:checked:hover { background-color: #966014; }"
        /* Keep window decorations light, including inactive windows. Override
         * the white foreground and black label backgrounds used in the body. */
        "headerbar, .titlebar {"
        " background-color: #e6e6e6; background-image: none; color: #000000;"
        " text-shadow: none; -gtk-icon-shadow: none; box-shadow: none; }"
        "headerbar *, .titlebar * {"
        " background-color: transparent; background-image: none; color: #000000;"
        " text-shadow: none; -gtk-icon-shadow: none; box-shadow: none; }"
        "headerbar button, .titlebar button { border-color: transparent; }"
        "headerbar button:hover, .titlebar button:hover { background-color: #d5d5d5; }"
        "headerbar button:active, .titlebar button:active { background-color: #c5c5c5; }";
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_data(provider, css, -1, &error)) {
        g_warning("Receiver theme: %s", error->message);
        g_clear_error(&error);
    } else {
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}
#endif
