// MIT License © 2025 Binary Dice Games
//
// Demo example: showcases every wish widget and layout type in a single window,
// analogous to ImGui::ShowDemoWindow for the wish framework.
//
// Usage: demo [--verbose] [--theme dark|light|classic]

#include "../common/example_app.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace bdg::bison;
using namespace bdg::bison::rmi::transport;
namespace wish = bdg::wish;

// ── Tab descriptors (one per TabBar entry) ────────────────────────────────────

static constexpr const char* kTabBasicsDesc = R"json(
        "tab_basics": { "type": "TabItem", "label": "Basics",
          "children": {
            "sec_labels":  { "type": "SeparatorText", "label": "Labels" },
            "lbl_static":  { "type": "Label", "text": "Static label — text set in JSON." },
            "lbl_dynamic": { "type": "Label", "text": "(updated by events)" },
            "lbl_clicks":  { "type": "Label", "text": "Click counter: 0" },
            "sec_buttons": { "type": "SeparatorText", "label": "Buttons" },
            "btn_row": {
              "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "btn_click": { "type": "Button", "label": "Click me"      },
                "btn_reset": { "type": "Button", "label": "Reset counter" },
                "btn_wide":  { "type": "Button", "label": "Wide button",  "width": 160 }
              }
            },
            "sec_checks": { "type": "SeparatorText", "label": "Checkboxes" },
            "chk_a":      { "type": "Checkbox", "label": "Option A", "value": false },
            "chk_b":      { "type": "Checkbox", "label": "Option B", "value": true  },
            "chk_vis":    { "type": "Checkbox", "label": "Show hidden label", "value": true },
            "lbl_hidden": { "type": "Label", "text": "    This label is toggled by the checkbox above." },
            "sec_radio":  { "type": "SeparatorText", "label": "Radio Buttons" },
            "radio_row": {
              "type": "HorizontalLayout", "spacing": 12,
              "children": {
                "rb_a": { "type": "RadioButton", "label": "Alpha",  "active": true  },
                "rb_b": { "type": "RadioButton", "label": "Beta",   "active": false },
                "rb_c": { "type": "RadioButton", "label": "Gamma",  "active": false }
              }
            }
          }
        })json";

static constexpr const char* kTabSlidersDesc = R"json(
        "tab_sliders": { "type": "TabItem", "label": "Sliders & Drags",
          "children": {
            "sec_sliders": { "type": "SeparatorText", "label": "Sliders" },
            "sf_opacity":  { "type": "SliderFloat", "label": "Opacity", "value": 1.0,  "min": 0.0,    "max": 1.0,   "format": "%.2f"     },
            "sf_angle":    { "type": "SliderFloat", "label": "Angle",   "value": 0.0,  "min": -180.0, "max": 180.0, "format": "%.0f deg" },
            "si_count":    { "type": "SliderInt",   "label": "Count",   "value": 10,   "min": 0,      "max": 50     },
            "sec_drags":   { "type": "SeparatorText", "label": "Drag Widgets" },
            "df_val":      { "type": "DragFloat", "label": "Float drag", "value": 1.5, "speed": 0.05, "min": 0.0, "max": 10.0, "format": "%.2f" },
            "di_val":      { "type": "DragInt",   "label": "Int drag",   "value": 42,  "speed": 0.5,  "min": 0,   "max": 200  }
          }
        })json";

static constexpr const char* kTabInputsDesc = R"json(
        "tab_inputs": { "type": "TabItem", "label": "Text & Numbers",
          "children": {
            "sec_text":  { "type": "SeparatorText", "label": "Text Input" },
            "txt_name":  { "type": "InputText", "label": "Name",    "value": "",            "hint": "Type your name..." },
            "txt_msg":   { "type": "InputText", "label": "Message", "value": "Hello, wish!" },
            "sec_nums":  { "type": "SeparatorText", "label": "Numeric Input" },
            "ii_qty":    { "type": "InputInt",   "label": "Quantity",  "value": 1,    "step": 1,    "step_fast": 10  },
            "if_price":  { "type": "InputFloat", "label": "Price ($)", "value": 9.99, "step": 0.01, "step_fast": 1.0, "format": "%.2f" }
          }
        })json";

static constexpr const char* kTabSelectionDesc = R"json(
        "tab_selection": { "type": "TabItem", "label": "Selection",
          "children": {
            "sec_combo": { "type": "SeparatorText", "label": "Combo Box" },
            "cmb_fruit": { "type": "Combo", "label": "Fruit",
                           "items": "Apple\nBanana\nCherry\nDate\nElder\nFig\nGrape", "value": 0 },
            "cmb_size":  { "type": "Combo", "label": "Size",
                           "items": "Small\nMedium\nLarge\nExtra Large", "value": 1 },
            "sec_sel":   { "type": "SeparatorText", "label": "Selectables" },
            "sel_a":     { "type": "Selectable", "label": "Item Alpha",  "selected": false },
            "sel_b":     { "type": "Selectable", "label": "Item Beta",   "selected": true  },
            "sel_c":     { "type": "Selectable", "label": "Item Gamma",  "selected": false },
            "sel_d":     { "type": "Selectable", "label": "Item Delta",  "selected": false }
          }
        })json";

static constexpr const char* kTabTreeDesc = R"json(
        "tab_tree": { "type": "TabItem", "label": "Tree & Collapse",
          "children": {
            "sec_tree": { "type": "SeparatorText", "label": "TreeNode" },
            "tn_root":  { "type": "TreeNode", "label": "Root node", "open": true,
              "children": {
                "tn_child_a": { "type": "TreeNode", "label": "Child A",
                  "children": {
                    "tn_leaf_1": { "type": "TreeNode", "label": "Leaf 1", "leaf": true },
                    "tn_leaf_2": { "type": "TreeNode", "label": "Leaf 2", "leaf": true }
                  }
                },
                "tn_child_b": { "type": "TreeNode", "label": "Child B",
                  "children": {
                    "tn_leaf_3": { "type": "TreeNode", "label": "Leaf 3", "leaf": true }
                  }
                }
              }
            },
            "sec_collap":  { "type": "SeparatorText", "label": "CollapsingHeader" },
            "ch_details":  { "type": "CollapsingHeader", "label": "Details",
              "children": {
                "ch_lbl_a": { "type": "Label", "text": "Line A inside collapsing header." },
                "ch_lbl_b": { "type": "Label", "text": "Line B inside collapsing header." }
              }
            },
            "ch_advanced": { "type": "CollapsingHeader", "label": "Advanced",
              "children": {
                "ch_lbl_c": { "type": "Label", "text": "Advanced options would live here." }
              }
            }
          }
        })json";

static constexpr const char* kTabMiscDesc = R"json(
        "tab_misc": { "type": "TabItem", "label": "Misc",
          "children": {
            "sec_progress": { "type": "SeparatorText", "label": "Progress Bar" },
            "pb_download":  { "type": "ProgressBar", "value": 0.65, "label": "65 %" },
            "pb_notext":    { "type": "ProgressBar", "value": 0.30 },
            "sec_layouts":  { "type": "SeparatorText", "label": "Layouts" },
            "lbl_hlay":     { "type": "Label", "text": "HorizontalLayout (spacing 12):" },
            "hlay": {
              "type": "HorizontalLayout", "spacing": 12,
              "children": {
                "hl_a": { "type": "Button", "label": "Alpha" },
                "hl_b": { "type": "Button", "label": "Beta"  },
                "hl_c": { "type": "Button", "label": "Gamma" }
              }
            },
            "lbl_vlay": { "type": "Label", "text": "VerticalLayout (spacing 4):" },
            "vlay": {
              "type": "VerticalLayout", "spacing": 4,
              "children": {
                "vr1": {
                  "type": "HorizontalLayout", "spacing": 6,
                  "children": {
                    "vr1c1": { "type": "Button", "label": "R1 C1" },
                    "vr1c2": { "type": "Button", "label": "R1 C2" },
                    "vr1c3": { "type": "Button", "label": "R1 C3" }
                  }
                },
                "vr2": {
                  "type": "HorizontalLayout", "spacing": 6,
                  "children": {
                    "vr2c1": { "type": "Button", "label": "R2 C1" },
                    "vr2c2": { "type": "Button", "label": "R2 C2" }
                  }
                }
              }
            },
            "lbl_split": { "type": "Label", "text": "Splitter (drag the bar):" },
            "split_wrap": {
              "type": "VerticalLayout",
              "children": {
                "split_demo": {
                  "type": "Splitter", "height": 120, "thickness": 6,
                  "children": {
                    "split_a": { "type": "Label", "text": "Left pane", "width": 150 },
                    "split_b": { "type": "Label", "text": "Right pane (fills remaining space)" }
                  }
                }
              }
            },
            "sec_theme": { "type": "SeparatorText", "label": "Theme" },
            "lbl_theme": { "type": "Label", "text": "Switch the visual theme at runtime:" },
            "theme_row": {
              "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "theme_dark":    { "type": "Button", "label": "Dark"    },
                "theme_light":   { "type": "Button", "label": "Light"   },
                "theme_classic": { "type": "Button", "label": "Classic" }
              }
            }
          }
        })json";

static constexpr const char* kTabTablesDesc = R"json(
        "tab_tables": { "type": "TabItem", "label": "Tables",
          "children": {
            "sec_static": { "type": "SeparatorText",
                            "label": "Static Table (Borders + Row Background + Resizable)" },
            "tbl_catalog": { "type": "Table", "id": "demo_catalog", "columns": 3,
                             "flags": 1985, "headers": true,
              "children": {
                "col_item":  { "type": "TableColumn", "label": "Item"     },
                "col_price": { "type": "TableColumn", "label": "Price"    },
                "col_cat":   { "type": "TableColumn", "label": "Category" },
                "row0": { "type": "TableRow", "children": {
                  "c0": { "type": "Label", "text": "Widget Alpha" },
                  "c1": { "type": "Label", "text": "$12.99"       },
                  "c2": { "type": "Label", "text": "Hardware"     }
                }},
                "row1": { "type": "TableRow", "children": {
                  "c0": { "type": "Label", "text": "Widget Beta"  },
                  "c1": { "type": "Label", "text": "$7.50"        },
                  "c2": { "type": "Label", "text": "Software"     }
                }},
                "row2": { "type": "TableRow", "children": {
                  "c0": { "type": "Label", "text": "Widget Gamma" },
                  "c1": { "type": "Label", "text": "$24.00"       },
                  "c2": { "type": "Label", "text": "Hardware"     }
                }},
                "row3": { "type": "TableRow", "children": {
                  "c0": { "type": "Label", "text": "Widget Delta" },
                  "c1": { "type": "Label", "text": "$3.25"        },
                  "c2": { "type": "Label", "text": "Consumable"   }
                }}
              }
            },
            "sec_inter": { "type": "SeparatorText",
                           "label": "Interactive Table (Buttons in Cells)" },
            "tbl_inter": { "type": "Table", "id": "demo_inter", "columns": 3,
                           "flags": 1921, "headers": true,
              "children": {
                "col_sensor": { "type": "TableColumn", "label": "Sensor",
                                "flags": 16, "init_width": 160 },
                "col_value":  { "type": "TableColumn", "label": "Value",
                                "flags": 16, "init_width": 120 },
                "col_action": { "type": "TableColumn", "label": "Action" },
                "row_temp": { "type": "TableRow", "children": {
                  "c0": { "type": "Label",  "text": "Temperature" },
                  "c1": { "type": "Label",  "text": "25.0 degC"   },
                  "c2": { "type": "Button", "label": "Read##temp"  }
                }},
                "row_hum": { "type": "TableRow", "children": {
                  "c0": { "type": "Label",  "text": "Humidity"    },
                  "c1": { "type": "Label",  "text": "60 % RH"     },
                  "c2": { "type": "Button", "label": "Read##hum"   }
                }},
                "row_pres": { "type": "TableRow", "children": {
                  "c0": { "type": "Label",  "text": "Pressure"    },
                  "c1": { "type": "Label",  "text": "1013 hPa"    },
                  "c2": { "type": "Button", "label": "Read##pres"  }
                }}
              }
            }
          }
        })json";

static constexpr const char* kTabPlotsDesc = R"json(
        "tab_plots": { "type": "TabItem", "label": "Plots",
          "children": {

            "ch_line": { "type": "CollapsingHeader", "label": "Line, Scatter & Stairs",
              "children": {
                "plt_lines": { "type": "Plot", "title": "Line Series",
                               "height": 220.0, "x_label": "x", "y_label": "y",
                  "children": {
                    "l_sin":    { "type": "PlotLine",    "label": "sin(x)"        },
                    "l_cos":    { "type": "PlotScatter", "label": "cos(x) noisy"  },
                    "l_stairs": { "type": "PlotStairs",  "label": "cos(x) stairs" }
                  }
                }
              }
            },

            "ch_area": { "type": "CollapsingHeader", "label": "Stems & Shaded Area",
              "children": {
                "plt_area": { "type": "Plot", "title": "Stems & Shaded",
                              "height": 220.0, "x_label": "x", "y_label": "y",
                  "children": {
                    "a_stems":  { "type": "PlotStems",  "label": "sin(x) stems"  },
                    "a_shaded": { "type": "PlotShaded", "label": "±0.4 band"     }
                  }
                }
              }
            },

            "ch_digital": { "type": "CollapsingHeader", "label": "Digital Signals",
              "children": {
                "plt_digital": { "type": "Plot", "title": "Digital Signals",
                                 "height": 160.0, "x_label": "t (s)",
                  "children": {
                    "d_pwm": { "type": "PlotDigital", "label": "PWM 67%"  },
                    "d_clk": { "type": "PlotDigital", "label": "Clock 50%" }
                  }
                }
              }
            },

            "ch_bars": { "type": "CollapsingHeader", "label": "Bar Charts",
              "children": {
                "plt_bars": { "type": "Plot", "title": "Vertical Bars",
                              "height": 200.0, "x_label": "Month", "y_label": "Units",
                  "children": {
                    "b_monthly": { "type": "PlotBars", "label": "Sales 2025" }
                  }
                },
                "plt_barsh": { "type": "Plot", "title": "Horizontal Bars",
                               "height": 180.0, "x_label": "Revenue ($k)", "y_label": "Quarter",
                  "children": {
                    "b_qtr": { "type": "PlotBarsH", "label": "Revenue" }
                  }
                }
              }
            },

            "ch_hist": { "type": "CollapsingHeader", "label": "Histograms",
              "children": {
                "plt_hist1": { "type": "Plot", "title": "1-D Histogram",
                               "height": 200.0, "x_label": "Value", "y_label": "Count",
                  "children": {
                    "h_norm": { "type": "PlotHistogram", "label": "Normal dist.", "bins": 40 }
                  }
                },
                "plt_hist2": { "type": "Plot", "title": "2-D Histogram",
                               "height": 220.0, "x_label": "X", "y_label": "Y",
                  "children": {
                    "h_2d": { "type": "PlotHistogram2D", "label": "Bivariate",
                              "x_bins": 25, "y_bins": 25 }
                  }
                }
              }
            },

            "ch_heatmap": { "type": "CollapsingHeader", "label": "Heatmap",
              "children": {
                "plt_heat": { "type": "Plot", "title": "Gaussian Heatmap",
                              "height": 260.0,
                  "children": {
                    "hm": { "type": "PlotHeatmap", "label": "Intensity",
                            "rows": 10, "cols": 10,
                            "scale_min": 0.0, "scale_max": 1.0, "format": "%.2f",
                            "x_max": 1.0, "y_max": 1.0 }
                  }
                }
              }
            },

            "ch_pie": { "type": "CollapsingHeader", "label": "Pie Chart",
              "children": {
                "plt_pie": { "type": "Plot", "title": "Market Share",
                             "height": 260.0,
                             "x_flags": 15, "y_flags": 15,
                  "children": {
                    "pie": { "type": "PlotPieChart", "label": "share",
                             "labels": "Web\nMobile\nDesktop\nTablet\nOther",
                             "normalize": true, "label_fmt": "%.0f%%",
                             "angle0": 90.0, "x": 0.5, "y": 0.5, "radius": 0.4 }
                  }
                }
              }
            },

            "ch_annot": { "type": "CollapsingHeader", "label": "Annotations",
              "children": {
                "plt_annot": { "type": "Plot", "title": "Annotations",
                               "height": 220.0, "x_label": "x", "y_label": "y",
                  "children": {
                    "an_line": { "type": "PlotLine",     "label": "sin(x)"   },
                    "an_txt":  { "type": "PlotText",     "text": "Peak",
                                 "x": 1.5708, "y": 1.05, "offset_y": -12.0  },
                    "an_vref": { "type": "PlotInfLines", "label": "x = π/2" },
                    "an_href": { "type": "PlotInfLines", "label": "y = 0",
                                 "horizontal": true                           }
                  }
                }
              }
            }

          }
        })json";

static constexpr const char* kTabPlot3DDesc = R"json(
        "tab_plot3d": { "type": "TabItem", "label": "3-D Plots",
          "children": {

            "ch3_line": { "type": "CollapsingHeader", "label": "Line & Scatter",
              "children": {
                "plt3_ls": { "type": "Plot3D", "title": "Helix & Sphere",
                             "height": 300.0,
                             "x_label": "X", "y_label": "Y", "z_label": "Z",
                  "children": {
                    "p3_helix":  { "type": "Plot3DLine",    "label": "Helix"            },
                    "p3_sphere": { "type": "Plot3DScatter", "label": "Fibonacci sphere" }
                  }
                }
              }
            },

            "ch3_surf": { "type": "CollapsingHeader", "label": "Surface",
              "children": {
                "plt3_surf": { "type": "Plot3D", "title": "Sinc Surface",
                               "height": 300.0,
                               "x_label": "X", "y_label": "Y", "z_label": "Z",
                  "children": {
                    "p3_sinc": { "type": "Plot3DSurface", "label": "sinc(r)",
                                 "x_count": 25, "y_count": 25,
                                 "scale_min": -0.25, "scale_max": 1.0 }
                  }
                }
              }
            },

            "ch3_mesh": { "type": "CollapsingHeader", "label": "Triangle, Quad & Mesh",
              "children": {
                "plt3_shapes": { "type": "Plot3D", "title": "3-D Shapes",
                                 "height": 320.0,
                  "children": {
                    "p3_tetra": { "type": "Plot3DTriangle", "label": "Tetrahedron" },
                    "p3_cube":  { "type": "Plot3DQuad",     "label": "Cube"        },
                    "p3_octa":  { "type": "Plot3DMesh",     "label": "Octahedron"  }
                  }
                }
              }
            },

            "ch3_text": { "type": "CollapsingHeader", "label": "Text Annotation",
              "children": {
                "plt3_text": { "type": "Plot3D", "title": "Axis Labels",
                               "height": 260.0,
                               "x_label": "X", "y_label": "Y", "z_label": "Z",
                  "children": {
                    "p3_axpts": { "type": "Plot3DScatter", "label": "Axis tips"    },
                    "p3_txt0":  { "type": "Plot3DText", "text": "Origin",
                                  "x": 0.0, "y": 0.0, "z": 0.0 },
                    "p3_txtx":  { "type": "Plot3DText", "text": "+X",
                                  "x": 1.0, "y": 0.0, "z": 0.0 },
                    "p3_txty":  { "type": "Plot3DText", "text": "+Y",
                                  "x": 0.0, "y": 1.0, "z": 0.0 },
                    "p3_txtz":  { "type": "Plot3DText", "text": "+Z",
                                  "x": 0.0, "y": 0.0, "z": 1.0 }
                  }
                }
              }
            }

          }
        })json";

static constexpr const char* kTabFilesDesc = R"json(
        "tab_files": { "type": "TabItem", "label": "Files",
          "children": {
            "sec_image":    { "type": "SeparatorText", "label": "Image Viewer" },
            "lbl_img_hint": { "type": "Label",
                              "text": "Enter a path to a BMP image file, or click ... to browse:" },
            "img_path_row": { "type": "HorizontalLayout", "spacing": 4,
              "children": {
                "txt_img_path":   { "type": "InputText", "label": "Image Path",
                                    "value": "", "max_length": 512 },
                "btn_img_browse": { "type": "Button", "label": "..." }
              }
            },
            "img_view":     { "type": "Image", "src": "", "width": 512, "height": 300 },
            "sec_editor":   { "type": "SeparatorText", "label": "Text / Code Editor" },
            "lbl_ed_hint":  { "type": "Label",
                              "text": "Enter a path to a text or code file, or click ... to browse:" },
            "ed_path_row":  { "type": "HorizontalLayout", "spacing": 4,
              "children": {
                "txt_ed_path":   { "type": "InputText", "label": "File Path",
                                   "value": "", "max_length": 512 },
                "btn_ed_browse": { "type": "Button", "label": "..." }
              }
            },
            "cmb_lang":     { "type": "Combo", "label": "Language",
                              "items": "none\ncpp\nc\ncs\nglsl\nhlsl\nlua\npython\nsql\njson\nmarkdown\nangelscript",
                              "value": 0 },
            "editor": { "type": "TextEditor", "file_path": "", "language": "none",
                        "height": 400 },
            "sec_font":       { "type": "SeparatorText", "label": "Font Override" },
            "lbl_font_hint":  { "type": "Label",
                                "text": "Enter a path to a TTF font file, or click ... to browse:" },
            "font_path_row":  { "type": "HorizontalLayout", "spacing": 4,
              "children": {
                "txt_font_path":   { "type": "InputText", "label": "Font Path",
                                     "value": "", "max_length": 512 },
                "btn_font_browse": { "type": "Button", "label": "..." }
              }
            },
            "sld_font_size":  { "type": "SliderFloat", "label": "Font Size (px)",
                                "value": 16.0, "min": 8.0, "max": 72.0 },
            "lbl_font_sample": { "type": "Label",
                                 "text": "The quick brown fox jumps over the lazy dog" }
          }
        })json";

static constexpr const char* kTabFormsDesc = R"json(
        "tab_forms": { "type": "TabItem", "label": "Forms",
          "children": {
            "sec_filedialog": { "type": "SeparatorText", "label": "File Dialog" },
            "lbl_filedialog_hint": { "type": "Label",
                                     "text": "Standalone file-picker dialog (the FileDialog form)." },
            "btn_open_filedialog": { "type": "Button", "label": "Open File Dialog..." },
            "sec_msgbox": { "type": "SeparatorText", "label": "Message Box" },
            "lbl_msgbox_hint": { "type": "Label",
                "text": "Win32-MessageBox-style modal dialog (the MessageBox form) -- click a button to show one:" },
            "msgbox_row1": { "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "btn_mb_ok":        { "type": "Button", "label": "OK"      },
                "btn_mb_ok_cancel": { "type": "Button", "label": "OK / Cancel" },
                "btn_mb_yes_no":    { "type": "Button", "label": "Yes / No"    }
              }
            },
            "msgbox_row2": { "type": "HorizontalLayout", "spacing": 8,
              "children": {
                "btn_mb_yes_no_cancel":       { "type": "Button", "label": "Yes / No / Cancel"       },
                "btn_mb_retry_cancel":        { "type": "Button", "label": "Retry / Cancel"          },
                "btn_mb_abort_retry_ignore":  { "type": "Button", "label": "Abort / Retry / Ignore"  }
              }
            }
          }
        })json";

// Escapes '"' and '\' for embedding an arbitrary string as a JSON string
// literal. Icon filenames come from the embedded resource archive (trusted,
// not user input), but this keeps the descriptor well-formed regardless.
static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '"' || c == '\\')
      out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

// Builds the "Icons" tab's descriptor with exactly one TableRow per entry in
// @p icon_files -- discovered at runtime (see on_session()'s list_files("res/icons")
// call) rather than hardcoded, so the tab always reflects whatever the
// embedded resource archive actually contains.
static std::string build_tab_icons_desc(const std::vector<std::string>& icon_files) {
  std::string rows;
  for (size_t i = 0; i < icon_files.size(); ++i) {
    const std::string& name = icon_files[i];
    std::string src = json_escape("res/icons/" + name);
    std::string label = json_escape(name);
    if (i > 0)
      rows += ",";
    rows += "\"row" + std::to_string(i) +
        "\": { \"type\": \"TableRow\", \"children\": {"
        "\"c0\": { \"type\": \"Image\", \"src\": \"" +
        src +
        "\", \"width\": 32, \"height\": 32 },"
        "\"c1\": { \"type\": \"Label\", \"text\": \"" +
        label + "\" } } }";
  }

  return R"json(
        "tab_icons": { "type": "TabItem", "label": "Icons",
          "children": {
            "sec_icons": { "type": "SeparatorText", "label": "Embedded Icons" },
            "lbl_icons_hint": { "type": "Label",
                                "text": "Built-in icons discovered at runtime under this session's res/icons/ folder (see src/resources/DESIGN.md)." },
            "tbl_icons": { "type": "Table", "id": "demo_icons", "columns": 2,
                           "flags": 1985, "headers": true,
              "children": {
                "col_icon": { "type": "TableColumn", "label": "Icon", "flags": 16, "init_width": 64 },
                "col_name": { "type": "TableColumn", "label": "File" },)json" +
      rows + R"json(
              }
            }
          }
        })json";
}

// ── Menu bar descriptor ────────────────────────────────────────────────────

static constexpr const char* kMenuBarDesc = R"json(
    "main_menu": { "type": "MenuBar",
      "children": {
        "m_file": { "type": "Menu", "label": "File",
          "children": {
            "mi_new":  { "type": "MenuItem", "label": "New",  "shortcut": "Ctrl+N" },
            "mi_open": { "type": "MenuItem", "label": "Open", "shortcut": "Ctrl+O" },
            "mi_sep":  { "type": "Separator" },
            "mi_quit": { "type": "MenuItem", "label": "Quit", "shortcut": "Alt+F4" }
          }
        },
        "m_view": { "type": "Menu", "label": "View",
          "children": {
            "mi_dark":    { "type": "MenuItem", "label": "Dark theme"    },
            "mi_light":   { "type": "MenuItem", "label": "Light theme"   },
            "mi_classic": { "type": "MenuItem", "label": "Classic theme" }
          }
        },
        "m_check": { "type": "Menu", "label": "Options",
          "children": {
            "mi_verbose": { "type": "MenuItem", "label": "Verbose logging", "checked": false }
          }
        }
      }
    })json";

// ── Full descriptor — assembled from per-tab pieces ─────────────────────────
//
// Root is a DockSpaceViewport so users can undock, resize, and rearrange panes.
// The menu bar lives at the viewport level; all widget content is inside
// demo_win, which ImGui can dock into the central dockspace node.
//
// Takes the Icons tab's descriptor as a parameter (rather than a static
// constant like every other tab) because it depends on a runtime directory
// listing -- see build_tab_icons_desc() and on_session()'s list_files() call.
static std::string build_demo_desc_str(const std::string& tab_icons_desc) {
  return
      // DockSpaceViewport root — menu bar floats at viewport level.
      std::string(R"json({
  "type": "DockSpaceViewport", "id": "demo_dockspace",
  "children": {)json") +
      kMenuBarDesc +
      ","
      // Main demo window: dockable, contains all tabs and the status bar.
      + R"json(
    "demo_win": { "type": "Window", "title": "wish Widget Demo",
      "width": 900, "height": 800,
      "children": {
        "tabs_root": { "type": "TabBar", "id": "demo_tabs",
          "children": {)json" +
      kTabBasicsDesc + "," + kTabSlidersDesc + "," + kTabInputsDesc + "," + kTabSelectionDesc + "," + kTabTreeDesc +
      "," + kTabMiscDesc + "," + kTabTablesDesc + "," + kTabPlotsDesc + "," + kTabPlot3DDesc + "," + kTabFilesDesc +
      "," + kTabFormsDesc + "," + tab_icons_desc
      // Close tab bar, add status bar, close window and dockspace.
      + R"json(

          }
        },
        "sep_status": { "type": "Separator" },
        "lbl_ev_hdr": { "type": "Label", "text": "Last event:" },
        "lbl_status": { "type": "Label", "text": "(interact with any widget to see its event here)" }
      }
    }
  }
})json";
}

// ── Plot data helpers ─────────────────────────────────────────────────────────

// Simple xorshift RNG for deterministic pseudo-random data.
static float rng_float(uint32_t& s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return (float(s & 0xFFFFu) / 32768.0f) - 1.0f; // [-1, 1)
}

// Box-Muller normal sample (returns z0; stores z1 via pointer for next call).
static float normal_sample(uint32_t& s) {
  const float pi2 = 6.28318530f;
  float u1 = (float((s ^= s << 13, s ^= s >> 17, s ^= s << 5, s) & 0xFFFFu) + 1.0f) / 65537.0f;
  float u2 = (float((s ^= s << 13, s ^= s >> 17, s ^= s << 5, s) & 0xFFFFu)) / 65536.0f;
  return std::sqrt(-2.0f * std::log(u1)) * std::cos(pi2 * u2);
}

// ── File-browser helpers ──────────────────────────────────────────────────────

// Build the `files` dynamic expected by FileDialog from a directory listing.
// The ".." entry is prepended when the path is not the filesystem root.
static dynamic list_directory(const std::filesystem::path& dir) {
  dynamic files;
  size_t i = 0;
  if (dir.has_parent_path() && dir != dir.root_path()) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = std::string{".."};
    (*e)["type"_key] = std::string{"dir"};
    files[i++] = dynamic_ptr{e};
  }
  std::error_code ec;
  for (auto& entry : std::filesystem::directory_iterator{dir, ec}) {
    auto e = std::make_shared<dynamic>();
    (*e)["name"_key] = entry.path().filename().string();
    (*e)["type"_key] = entry.is_directory(ec) ? std::string{"dir"} : std::string{"file"};
    files[i++] = dynamic_ptr{e};
  }
  return files;
}

// ── Demo client ───────────────────────────────────────────────────────────────

class demo_client : public wish::examples::example_client {
 public:
  demo_client(memory_client_transport t, wish::renderer* renderer, bool verbose = false, std::string theme = "dark")
      : wish::examples::example_client(std::move(t), renderer, verbose, std::move(theme), "demo") {}

 protected:
  void on_session() override {
    vlog("applying " + theme_ + " theme");
    set_style_preset(theme_).get();

    vlog("discovering embedded icon resources");
    auto icon_files = list_files("res/icons").get();
    std::sort(icon_files.begin(), icon_files.end()); // stable, deterministic row order
    vlog("found " + std::to_string(icon_files.size()) + " icon(s) in res/icons");

    vlog("registering and instantiating template");
    register_template_from_json("demo"_key, build_demo_desc_str(build_tab_icons_desc(icon_files))).get();
    auto pm = instantiate_template("demo"_key).get();

    // ── Shared helpers ────────────────────────────────────────────────────

    auto set_text = [&](const std::string& name, const std::string& text) {
      dynamic f;
      f["text"_key] = text;
      pm.at(name).set(std::move(f));
    };

    auto set_visible = [&](const std::string& name, bool vis) {
      dynamic f;
      f["visible"_key] = vis;
      pm.at(name).set(std::move(f));
    };

    auto set_xy = [&](const std::string& path, std::vector<float> xs, std::vector<float> ys) {
      dynamic d;
      d["xs"_key] = std::move(xs);
      d["ys"_key] = std::move(ys);
      pm.at(path).set(std::move(d));
    };

    auto status = [&, set_text](const std::string& msg) {
      vlog("event: " + msg);
      set_text("demo_win.lbl_status", msg);
    };

    // ── Plot data ─────────────────────────────────────────────────────────

    // Sinusoidal data (100 points over 0..2π).
    constexpr int kN = 100;
    const float k2pi = 6.28318530f;
    std::vector<float> xs(kN), sin_ys(kN), cos_ys(kN);
    for (int i = 0; i < kN; ++i) {
      float t = k2pi * float(i) / float(kN - 1);
      xs[i] = t;
      sin_ys[i] = std::sin(t);
      cos_ys[i] = std::cos(t);
    }

    // Noisy cosine for scatter.
    uint32_t rng = 0xDEADBEEFu;
    std::vector<float> noisy_cos(kN);
    for (int i = 0; i < kN; ++i)
      noisy_cos[i] = cos_ys[i] + rng_float(rng) * 0.35f;

    // Section 1: Line, Scatter, Stairs.
    set_xy("demo_win.tabs_root.tab_plots.ch_line.plt_lines.l_sin", xs, sin_ys);
    set_xy("demo_win.tabs_root.tab_plots.ch_line.plt_lines.l_cos", xs, noisy_cos);
    set_xy("demo_win.tabs_root.tab_plots.ch_line.plt_lines.l_stairs", xs, cos_ys);

    // Section 2: Stems and Shaded.
    set_xy("demo_win.tabs_root.tab_plots.ch_area.plt_area.a_stems", xs, sin_ys);
    {
      std::vector<float> band_hi(kN), band_lo(kN);
      for (int i = 0; i < kN; ++i) {
        band_hi[i] = sin_ys[i] + 0.4f;
        band_lo[i] = sin_ys[i] - 0.4f;
      }
      dynamic d;
      d["xs"_key] = xs;
      d["ys"_key] = band_hi;
      d["ys2"_key] = band_lo;
      pm.at("demo_win.tabs_root.tab_plots.ch_area.plt_area.a_shaded").set(std::move(d));
    }

    // Section 3: Digital signals (200 steps at 20 Hz → 10 s window).
    {
      constexpr int kD = 200;
      std::vector<float> dt(kD), pwm(kD), clk(kD);
      for (int i = 0; i < kD; ++i) {
        dt[i] = float(i) * 0.05f;
        pwm[i] = (i % 6 < 4) ? 1.0f : 0.0f; // 67 % duty cycle
        clk[i] = (i % 2 == 0) ? 1.0f : 0.0f; // 50 % clock
      }
      set_xy("demo_win.tabs_root.tab_plots.ch_digital.plt_digital.d_pwm", dt, pwm);
      set_xy("demo_win.tabs_root.tab_plots.ch_digital.plt_digital.d_clk", dt, clk);
    }

    // Section 4: Bar charts (12 monthly sales, 4 quarterly revenues).
    {
      std::vector<float> month_xs, month_ys;
      const float sales[] = {42, 38, 61, 54, 72, 80, 65, 59, 78, 91, 83, 76};
      for (int i = 0; i < 12; ++i) {
        month_xs.push_back(float(i + 1));
        month_ys.push_back(sales[i]);
      }
      set_xy("demo_win.tabs_root.tab_plots.ch_bars.plt_bars.b_monthly", month_xs, month_ys);

      // Horizontal bars: ys = quarter index, xs = revenue
      std::vector<float> qtr_ys = {1.0f, 2.0f, 3.0f, 4.0f};
      std::vector<float> qtr_xs = {42.5f, 38.2f, 51.0f, 45.7f};
      set_xy("demo_win.tabs_root.tab_plots.ch_bars.plt_barsh.b_qtr", qtr_xs, qtr_ys);
    }

    // Section 5: Histograms (1000 normal-distribution samples).
    {
      constexpr int kS = 1000;
      rng = 0x12345678u;
      std::vector<float> h_vals(kS), h_xs(kS), h_ys(kS);
      for (int i = 0; i < kS; ++i) {
        float z = normal_sample(rng);
        h_vals[i] = z;
        h_xs[i] = z;
        // Correlated second variable for 2-D histogram.
        h_ys[i] = 0.7f * z + 0.3f * normal_sample(rng);
      }
      {
        dynamic d;
        d["values"_key] = h_vals;
        pm.at("demo_win.tabs_root.tab_plots.ch_hist.plt_hist1.h_norm").set(std::move(d));
      }
      set_xy("demo_win.tabs_root.tab_plots.ch_hist.plt_hist2.h_2d", h_xs, h_ys);
    }

    // Section 6: Heatmap — 10×10 Gaussian bell.
    {
      constexpr int kR = 10, kC = 10;
      std::vector<float> heat(kR * kC);
      for (int r = 0; r < kR; ++r) {
        for (int c = 0; c < kC; ++c) {
          float x = (float(c) - float(kC - 1) * 0.5f) / float(kC) * 4.0f;
          float y = (float(r) - float(kR - 1) * 0.5f) / float(kR) * 4.0f;
          heat[r * kC + c] = std::exp(-(x * x + y * y) * 0.5f);
        }
      }
      dynamic d;
      d["values"_key] = heat;
      pm.at("demo_win.tabs_root.tab_plots.ch_heatmap.plt_heat.hm").set(std::move(d));
    }

    // Section 7: Pie chart values (Web, Mobile, Desktop, Tablet, Other).
    {
      dynamic d;
      d["values"_key] = std::vector<float>{38.0f, 28.0f, 22.0f, 8.0f, 4.0f};
      pm.at("demo_win.tabs_root.tab_plots.ch_pie.plt_pie.pie").set(std::move(d));
    }

    // Section 8: Annotations — sin(x) line + text label + inf lines.
    set_xy("demo_win.tabs_root.tab_plots.ch_annot.plt_annot.an_line", xs, sin_ys);
    {
      // Vertical reference at x = π/2.
      dynamic dv;
      dv["values"_key] = std::vector<float>{k2pi / 4.0f};
      pm.at("demo_win.tabs_root.tab_plots.ch_annot.plt_annot.an_vref").set(std::move(dv));
    }
    {
      // Horizontal reference at y = 0.
      dynamic dh;
      dh["values"_key] = std::vector<float>{0.0f};
      pm.at("demo_win.tabs_root.tab_plots.ch_annot.plt_annot.an_href").set(std::move(dh));
    }

    // ── Plot3D data ───────────────────────────────────────────────────────

    // A: Helix line + Fibonacci sphere scatter.
    {
      constexpr int kHL = 100;
      std::vector<float> hx(kHL), hy(kHL), hz(kHL);
      for (int i = 0; i < kHL; ++i) {
        float t = 4.0f * k2pi * float(i) / float(kHL - 1);
        hx[i] = std::cos(t);
        hy[i] = std::sin(t);
        hz[i] = t / (4.0f * k2pi);
      }
      {
        dynamic d;
        d["xs"_key] = hx;
        d["ys"_key] = hy;
        d["zs"_key] = hz;
        pm.at("demo_win.tabs_root.tab_plot3d.ch3_line.plt3_ls.p3_helix").set(std::move(d));
      }

      constexpr int kFib = 60;
      const float kGolden = k2pi * (1.0f - 0.618033988f);
      std::vector<float> spx(kFib), spy(kFib), spz(kFib);
      for (int i = 0; i < kFib; ++i) {
        float inc = std::acos(1.0f - 2.0f * float(i) / float(kFib));
        float az = kGolden * float(i);
        spx[i] = std::sin(inc) * std::cos(az);
        spy[i] = std::sin(inc) * std::sin(az);
        spz[i] = std::cos(inc);
      }
      {
        dynamic d;
        d["xs"_key] = spx;
        d["ys"_key] = spy;
        d["zs"_key] = spz;
        pm.at("demo_win.tabs_root.tab_plot3d.ch3_line.plt3_ls.p3_sphere").set(std::move(d));
      }
    }

    // B: Sinc surface on 25×25 grid.
    {
      constexpr int kNS = 25;
      std::vector<float> sx(kNS * kNS), sy(kNS * kNS), sz(kNS * kNS);
      for (int i = 0; i < kNS; ++i) {
        for (int j = 0; j < kNS; ++j) {
          float x = -4.0f + 8.0f * float(j) / float(kNS - 1);
          float y = -4.0f + 8.0f * float(i) / float(kNS - 1);
          float r = std::sqrt(x * x + y * y) + 1e-6f;
          sx[i * kNS + j] = x;
          sy[i * kNS + j] = y;
          sz[i * kNS + j] = std::sin(r) / r;
        }
      }
      dynamic d;
      d["xs"_key] = sx;
      d["ys"_key] = sy;
      d["zs"_key] = sz;
      pm.at("demo_win.tabs_root.tab_plot3d.ch3_surf.plt3_surf.p3_sinc").set(std::move(d));
    }

    // C: 3-D shapes — tetrahedron (triangles), cube (quads), octahedron (mesh).
    // Each shape is offset along X so they don't overlap in the shared plot.
    {
      // Tetrahedron — 4 faces × 3 vertices, offset to x−3.
      const float tv[4][3] = {{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}};
      const int tf[4][3] = {{0, 1, 2}, {0, 3, 1}, {0, 2, 3}, {1, 3, 2}};
      std::vector<float> tri_xs, tri_ys, tri_zs;
      for (auto& f : tf)
        for (int v : f) {
          tri_xs.push_back(tv[v][0] * 0.8f - 3.0f);
          tri_ys.push_back(tv[v][1] * 0.8f);
          tri_zs.push_back(tv[v][2] * 0.8f);
        }
      {
        dynamic d;
        d["xs"_key] = tri_xs;
        d["ys"_key] = tri_ys;
        d["zs"_key] = tri_zs;
        pm.at("demo_win.tabs_root.tab_plot3d.ch3_mesh.plt3_shapes.p3_tetra").set(std::move(d));
      }

      // Cube — 6 faces × 4 vertices, centered at origin.
      const float cv[8][3] = {
          {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}};
      const int qf[6][4] = {{0, 1, 2, 3}, {7, 6, 5, 4}, {0, 4, 5, 1}, {1, 5, 6, 2}, {2, 6, 7, 3}, {3, 7, 4, 0}};
      std::vector<float> quad_xs, quad_ys, quad_zs;
      for (auto& f : qf)
        for (int v : f) {
          quad_xs.push_back(cv[v][0] * 0.8f);
          quad_ys.push_back(cv[v][1] * 0.8f);
          quad_zs.push_back(cv[v][2] * 0.8f);
        }
      {
        dynamic d;
        d["xs"_key] = quad_xs;
        d["ys"_key] = quad_ys;
        d["zs"_key] = quad_zs;
        pm.at("demo_win.tabs_root.tab_plot3d.ch3_mesh.plt3_shapes.p3_cube").set(std::move(d));
      }

      // Octahedron — 6 vertices, 8 triangular faces, offset to x+3.
      const float ov[6][3] = {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}, {-1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
      const int oi[8][3] = {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 1}, {5, 2, 1}, {5, 3, 2}, {5, 4, 3}, {5, 1, 4}};
      std::vector<float> mxs, mys, mzs;
      std::vector<int32_t> midxs;
      for (int v = 0; v < 6; ++v) {
        mxs.push_back(ov[v][0] * 0.8f + 3.0f);
        mys.push_back(ov[v][1] * 0.8f);
        mzs.push_back(ov[v][2] * 0.8f);
      }
      for (auto& f : oi)
        for (int v : f)
          midxs.push_back(int32_t(v));
      {
        dynamic d;
        d["xs"_key] = mxs;
        d["ys"_key] = mys;
        d["zs"_key] = mzs;
        d["indices"_key] = midxs;
        pm.at("demo_win.tabs_root.tab_plot3d.ch3_mesh.plt3_shapes.p3_octa").set(std::move(d));
      }
    }

    // D: Text annotation — scatter at axis tips (origin + unit X/Y/Z).
    {
      dynamic d;
      d["xs"_key] = std::vector<float>{0.0f, 1.0f, 0.0f, 0.0f};
      d["ys"_key] = std::vector<float>{0.0f, 0.0f, 1.0f, 0.0f};
      d["zs"_key] = std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f};
      pm.at("demo_win.tabs_root.tab_plot3d.ch3_text.plt3_text.p3_axpts").set(std::move(d));
    }

    // ── Basics tab: buttons, checkboxes, radio ────────────────────────────

    pm.at("demo_win.tabs_root.tab_basics.btn_row.btn_click").onEvent("clicked"_key, [&, set_text, status](dynamic) {
      ++click_count_;
      set_text("demo_win.tabs_root.tab_basics.lbl_clicks", "Click counter: " + std::to_string(click_count_));
      status("'Click me' pressed (" + std::to_string(click_count_) + " times total)");
    });

    pm.at("demo_win.tabs_root.tab_basics.btn_row.btn_reset").onEvent("clicked"_key, [&, set_text, status](dynamic) {
      click_count_ = 0;
      set_text("demo_win.tabs_root.tab_basics.lbl_clicks", "Click counter: 0");
      status("Counter reset.");
    });

    pm.at("demo_win.tabs_root.tab_basics.btn_row.btn_wide").onEvent("clicked"_key, [status](dynamic) {
      status("Wide button clicked.");
    });

    pm.at("demo_win.tabs_root.tab_basics.chk_a").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
      status("Option A: " + std::string(v ? "checked" : "unchecked"));
    });

    pm.at("demo_win.tabs_root.tab_basics.chk_b").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
      status("Option B: " + std::string(v ? "checked" : "unchecked"));
    });

    pm.at("demo_win.tabs_root.tab_basics.chk_vis").onEvent("changed"_key, [set_visible, status](dynamic p) {
      const auto* f = p.findField("value"_key);
      bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
      set_visible("demo_win.tabs_root.tab_basics.lbl_hidden", v);
      status("Visibility: label is now " + std::string(v ? "shown" : "hidden"));
    });

    static const char* kRBNames[] = {
        "demo_win.tabs_root.tab_basics.radio_row.rb_a",
        "demo_win.tabs_root.tab_basics.radio_row.rb_b",
        "demo_win.tabs_root.tab_basics.radio_row.rb_c"};
    static const char* kRBLabels[] = {"Alpha", "Beta", "Gamma"};
    for (int i = 0; i < 3; ++i) {
      pm.at(kRBNames[i]).onEvent("clicked"_key, [&, i, status](dynamic) {
        for (int j = 0; j < 3; ++j) {
          dynamic f;
          f["active"_key] = (j == i);
          pm.at(kRBNames[j]).set(std::move(f));
        }
        status(std::string("Radio: ") + kRBLabels[i] + " selected.");
      });
    }

    // ── Sliders & Drags tab ───────────────────────────────────────────────

    pm.at("demo_win.tabs_root.tab_sliders.sf_opacity").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "Opacity: %.2f", v);
      status(buf);
    });
    pm.at("demo_win.tabs_root.tab_sliders.sf_angle").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "Angle: %.0f deg", v);
      status(buf);
    });
    pm.at("demo_win.tabs_root.tab_sliders.si_count").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
      status("Count: " + std::to_string(v));
    });
    pm.at("demo_win.tabs_root.tab_sliders.df_val").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "Float drag: %.2f", v);
      status(buf);
    });
    pm.at("demo_win.tabs_root.tab_sliders.di_val").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
      status("Int drag: " + std::to_string(v));
    });

    // ── Text & Numbers tab ────────────────────────────────────────────────

    pm.at("demo_win.tabs_root.tab_inputs.txt_name").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      status("Name: \"" + v + "\"");
    });
    pm.at("demo_win.tabs_root.tab_inputs.txt_msg").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      status("Message: \"" + v + "\"");
    });
    pm.at("demo_win.tabs_root.tab_inputs.ii_qty").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      int32_t v = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
      status("Quantity: " + std::to_string(v));
    });
    pm.at("demo_win.tabs_root.tab_inputs.if_price").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("value"_key);
      float v = (f && f->is<float>()) ? f->as<float>() : 0.0f;
      char buf[64];
      std::snprintf(buf, sizeof(buf), "Price: $%.2f", v);
      status(buf);
    });

    // ── Selection tab ─────────────────────────────────────────────────────

    pm.at("demo_win.tabs_root.tab_selection.cmb_fruit").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("text"_key);
      std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "?";
      status("Fruit: " + v);
    });
    pm.at("demo_win.tabs_root.tab_selection.cmb_size").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("text"_key);
      std::string v = (f && f->is<std::string>()) ? f->as<std::string>() : "?";
      status("Size: " + v);
    });

    static const char* kSelNames[] = {
        "demo_win.tabs_root.tab_selection.sel_a",
        "demo_win.tabs_root.tab_selection.sel_b",
        "demo_win.tabs_root.tab_selection.sel_c",
        "demo_win.tabs_root.tab_selection.sel_d"};
    static const char* kSelLabels[] = {"Alpha", "Beta", "Gamma", "Delta"};
    for (int i = 0; i < 4; ++i) {
      pm.at(kSelNames[i]).onEvent("changed"_key, [i, status](dynamic p) {
        const auto* f = p.findField("selected"_key);
        bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
        status(std::string("Selectable ") + kSelLabels[i] + ": " + (v ? "selected" : "deselected"));
      });
    }

    // ── Tree tab ──────────────────────────────────────────────────────────

    auto on_toggled = [status](const std::string& name) {
      return [name, status](dynamic p) {
        const auto* f = p.findField("open"_key);
        bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
        status(name + ": " + (v ? "expanded" : "collapsed"));
      };
    };
    pm.at("demo_win.tabs_root.tab_tree.tn_root").onEvent("toggled"_key, on_toggled("Root node"));
    pm.at("demo_win.tabs_root.tab_tree.tn_root.tn_child_a").onEvent("toggled"_key, on_toggled("Child A"));
    pm.at("demo_win.tabs_root.tab_tree.tn_root.tn_child_b").onEvent("toggled"_key, on_toggled("Child B"));
    pm.at("demo_win.tabs_root.tab_tree.ch_details").onEvent("toggled"_key, on_toggled("CollapsingHeader: Details"));
    pm.at("demo_win.tabs_root.tab_tree.ch_advanced").onEvent("toggled"_key, on_toggled("CollapsingHeader: Advanced"));

    // ── Misc tab ──────────────────────────────────────────────────────────

    for (const auto* name :
         {"demo_win.tabs_root.tab_misc.hlay.hl_a",
          "demo_win.tabs_root.tab_misc.hlay.hl_b",
          "demo_win.tabs_root.tab_misc.hlay.hl_c",
          "demo_win.tabs_root.tab_misc.vlay.vr1.vr1c1",
          "demo_win.tabs_root.tab_misc.vlay.vr1.vr1c2",
          "demo_win.tabs_root.tab_misc.vlay.vr1.vr1c3",
          "demo_win.tabs_root.tab_misc.vlay.vr2.vr2c1",
          "demo_win.tabs_root.tab_misc.vlay.vr2.vr2c2"}) {
      pm.at(name).onEvent(
          "clicked"_key, [s = status, n = std::string(name)](dynamic) { s("Layout button '" + n + "' clicked."); });
    }

    pm.at("demo_win.tabs_root.tab_misc.split_wrap.split_demo").onEvent("resized"_key, [status](dynamic p) {
      const auto* s1 = p.findField("size1"_key);
      const auto* s2 = p.findField("size2"_key);
      float v1 = (s1 && s1->is<float>()) ? s1->as<float>() : 0.0f;
      float v2 = (s2 && s2->is<float>()) ? s2->as<float>() : 0.0f;
      status("Splitter resized: " + std::to_string(int(v1)) + "px / " + std::to_string(int(v2)) + "px");
    });

    pm.at("demo_win.tabs_root.tab_misc.theme_row.theme_dark").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("dark").get();
      status("Theme: dark");
    });
    pm.at("demo_win.tabs_root.tab_misc.theme_row.theme_light").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("light").get();
      status("Theme: light");
    });
    pm.at("demo_win.tabs_root.tab_misc.theme_row.theme_classic").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("classic").get();
      status("Theme: classic");
    });

    // ── Tables tab ────────────────────────────────────────────────────────

    pm.at("demo_win.tabs_root.tab_tables.tbl_inter.row_temp.c2").onEvent("clicked"_key, [status](dynamic) {
      status("Table: Read Temperature");
    });
    pm.at("demo_win.tabs_root.tab_tables.tbl_inter.row_hum.c2").onEvent("clicked"_key, [status](dynamic) {
      status("Table: Read Humidity");
    });
    pm.at("demo_win.tabs_root.tab_tables.tbl_inter.row_pres.c2").onEvent("clicked"_key, [status](dynamic) {
      status("Table: Read Pressure");
    });

    // ── Menu bar ──────────────────────────────────────────────────────────

    pm.at("main_menu.m_file.mi_new").onEvent("clicked"_key, [status](dynamic) { status("Menu: File > New"); });
    pm.at("main_menu.m_file.mi_open").onEvent("clicked"_key, [status](dynamic) { status("Menu: File > Open"); });
    pm.at("main_menu.m_file.mi_quit").onEvent("clicked"_key, [status](dynamic) { status("Menu: File > Quit"); });
    pm.at("main_menu.m_view.mi_dark").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("dark").get();
      status("Menu: View > Dark theme");
    });
    pm.at("main_menu.m_view.mi_light").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("light").get();
      status("Menu: View > Light theme");
    });
    pm.at("main_menu.m_view.mi_classic").onEvent("clicked"_key, [&, status](dynamic) {
      set_style_preset("classic").get();
      status("Menu: View > Classic theme");
    });
    pm.at("main_menu.m_check.mi_verbose").onEvent("clicked"_key, [status](dynamic p) {
      const auto* f = p.findField("checked"_key);
      bool v = (f && f->is<bool>()) ? f->as<bool>() : false;
      status(std::string("Verbose logging: ") + (v ? "on" : "off"));
    });

    // ── Files tab ─────────────────────────────────────────────────────────

    // Image path: forward changes to the Image widget's src field.
    pm.at("demo_win.tabs_root.tab_files.img_path_row.txt_img_path").onEvent("changed"_key, [&pm, status](dynamic p) {
      const auto* f = p.findField("value"_key);
      std::string path = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      dynamic fields;
      fields["src"_key] = path;
      pm.at("demo_win.tabs_root.tab_files.img_view").set(std::move(fields));
      status("Image path: " + path);
    });

    // Editor path: forward changes to the TextEditor's file_path field.
    pm.at("demo_win.tabs_root.tab_files.ed_path_row.txt_ed_path").onEvent("changed"_key, [&pm, status](dynamic p) {
      const auto* f = p.findField("value"_key);
      std::string path = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      dynamic fields;
      fields["file_path"_key] = path;
      pm.at("demo_win.tabs_root.tab_files.editor").set(std::move(fields));
      status("Editor path: " + path);
    });

    // Language combo: map item index to language string.
    static const char* kLangNames[] = {
        "none", "cpp", "c", "cs", "glsl", "hlsl", "lua", "python", "sql", "json", "markdown", "angelscript"};
    pm.at("demo_win.tabs_root.tab_files.cmb_lang").onEvent("changed"_key, [&, status](dynamic p) {
      const auto* f = p.findField("value"_key);
      int32_t idx = (f && f->is<int32_t>()) ? f->as<int32_t>() : 0;
      if (idx < 0 || idx >= 12)
        idx = 0;
      dynamic fields;
      fields["language"_key] = std::string(kLangNames[idx]);
      pm.at("demo_win.tabs_root.tab_files.editor").set(std::move(fields));
      status(std::string("Language: ") + kLangNames[idx]);
    });

    // TextEditor events.
    pm.at("demo_win.tabs_root.tab_files.editor").onEvent("changed"_key, [status](dynamic p) {
      const auto* f = p.findField("file_path"_key);
      std::string path = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      status("File modified: " + path);
    });
    pm.at("demo_win.tabs_root.tab_files.editor").onEvent("saved"_key, [status](dynamic p) {
      const auto* f = p.findField("file_path"_key);
      std::string path = (f && f->is<std::string>()) ? f->as<std::string>() : "";
      status("File saved: " + path);
    });

    // Font path and size: both fields are sent together so the widget always
    // has a consistent (path, size) pair — a non-zero size with no path (or
    // vice versa) would silently use the default font.
    auto font_path_ptr = std::make_shared<std::string>("");
    auto font_size_ptr = std::make_shared<float>(16.0f);

    pm.at("demo_win.tabs_root.tab_files.font_path_row.txt_font_path")
        .onEvent("changed"_key, [&pm, status, font_path_ptr, font_size_ptr](dynamic p) {
          const auto* f = p.findField("value"_key);
          *font_path_ptr = (f && f->is<std::string>()) ? f->as<std::string>() : "";
          dynamic fields;
          fields["font_path"_key] = *font_path_ptr;
          fields["font_size"_key] = *font_size_ptr;
          pm.at("demo_win.tabs_root.tab_files.lbl_font_sample").set(std::move(fields));
          status("Font path: " + *font_path_ptr);
        });

    pm.at("demo_win.tabs_root.tab_files.sld_font_size")
        .onEvent("changed"_key, [&pm, status, font_path_ptr, font_size_ptr](dynamic p) {
          const auto* f = p.findField("value"_key);
          *font_size_ptr = (f && f->is<float>()) ? f->as<float>() : 16.0f;
          dynamic fields;
          fields["font_path"_key] = *font_path_ptr;
          fields["font_size"_key] = *font_size_ptr;
          pm.at("demo_win.tabs_root.tab_files.lbl_font_sample").set(std::move(fields));
          status("Font size: " + std::to_string(static_cast<int>(*font_size_ptr)) + "px");
        });

    // ── Browse buttons (open FileDialog to pick each path) ───────────────

    // Creates a FileDialog starting in CWD. The `on_selected` callback
    // receives the chosen absolute path; browse() returns immediately.
    // Each filter entry is a {label, regex} pair where label is shown in the
    // combo box and regex is applied to filenames (empty regex = match all).
    using filter_list = std::vector<std::pair<std::string, std::string>>;
    auto browse = [this](
                      const std::string& title,
                      const std::string& confirm_label,
                      const filter_list& filters,
                      std::function<void(std::string)> on_selected) {
      namespace fs = std::filesystem;
      auto cur_dir = std::make_shared<fs::path>(fs::current_path());

      auto raw_dlg = this->instantiate("wish"_key, "FileDialog"_key).get();
      auto dlg = std::make_shared<bdg::bison::rmi::proxy::dynamic>(std::move(raw_dlg));

      dynamic init;
      init["title"_key] = title;
      init["confirm_label"_key] = confirm_label;
      init["path"_key] = cur_dir->string();
      init["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory(*cur_dir))};

      // Populate the filter combo if caller supplied filter entries.
      if (!filters.empty()) {
        dynamic filter_items;
        for (size_t fi = 0; fi < filters.size(); ++fi) {
          auto entry = dynamic_ptr{std::make_shared<dynamic>()};
          (*entry)["label"_key] = filters[fi].first;
          (*entry)["regex"_key] = filters[fi].second;
          filter_items[fi] = std::move(entry);
        }
        init["filters"_key] = dynamic_ptr{std::make_shared<dynamic>(std::move(filter_items))};
      }

      dlg->set(std::move(init)).get();

      dlg->onEvent("on_navigate"_key, [dlg, cur_dir](dynamic payload) mutable {
        auto name = payload.as<std::string>("name"_key);
        auto type = payload.as<std::string>("type"_key);
        // type="path" means the user typed an absolute/arbitrary path.
        if (type == "path") {
          *cur_dir = fs::path(name);
        } else {
          *cur_dir = (name == "..") ? cur_dir->parent_path() : (*cur_dir / name);
        }
        dynamic fields;
        fields["path"_key] = cur_dir->string();
        fields["files"_key] = dynamic_ptr{std::make_shared<dynamic>(list_directory(*cur_dir))};
        dlg->set(std::move(fields));
      });

      dlg->onEvent("on_open"_key, [cur_dir, on_selected](dynamic payload) {
        auto name = payload.as<std::string>("path"_key);
        fs::path full = fs::path(name).is_absolute() ? fs::path(name) : (*cur_dir / name);
        on_selected(full.string());
      });

      // on_cancel: dialog already removed itself from session.objects; nothing
      // else is needed here, but the capture keeps dlg alive until this fires.
      dlg->onEvent("on_cancel"_key, [dlg](dynamic) {});
    };

    pm.at("demo_win.tabs_root.tab_files.img_path_row.btn_img_browse")
        .onEvent("clicked"_key, [&pm, status, browse](dynamic) {
          browse(
              "Select Image",
              "Open",
              {{"Images (*.bmp *.png *.jpg)", "\\.(bmp|png|jpg)$"}, {"All Files (*.*)", ""}},
              [&pm, status](std::string path) {
                dynamic f;
                f["value"_key] = path;
                pm.at("demo_win.tabs_root.tab_files.img_path_row.txt_img_path").set(f);
                dynamic g;
                g["src"_key] = path;
                pm.at("demo_win.tabs_root.tab_files.img_view").set(g);
                status("Image path: " + path);
              });
        });

    pm.at("demo_win.tabs_root.tab_files.ed_path_row.btn_ed_browse")
        .onEvent("clicked"_key, [&pm, status, browse](dynamic) {
          browse(
              "Select File",
              "Open",
              {{"Source Files (*.cpp *.hpp *.c *.h)", "\\.(cpp|hpp|c|h)$"},
               {"Text Files (*.txt *.md)", "\\.(txt|md)$"},
               {"All Files (*.*)", ""}},
              [&pm, status](std::string path) {
                dynamic f;
                f["value"_key] = path;
                pm.at("demo_win.tabs_root.tab_files.ed_path_row.txt_ed_path").set(f);
                dynamic g;
                g["file_path"_key] = path;
                pm.at("demo_win.tabs_root.tab_files.editor").set(g);
                status("Editor path: " + path);
              });
        });

    pm.at("demo_win.tabs_root.tab_files.font_path_row.btn_font_browse")
        .onEvent("clicked"_key, [&pm, status, browse, font_path_ptr, font_size_ptr](dynamic) {
          browse(
              "Select Font",
              "Select",
              {{"TrueType Fonts (*.ttf)", "\\.ttf$"}, {"OpenType Fonts (*.otf)", "\\.otf$"}, {"All Files (*.*)", ""}},
              [&pm, status, font_path_ptr, font_size_ptr](std::string path) {
                *font_path_ptr = path;
                dynamic f;
                f["value"_key] = path;
                pm.at("demo_win.tabs_root.tab_files.font_path_row.txt_font_path").set(f);
                dynamic g;
                g["font_path"_key] = *font_path_ptr;
                g["font_size"_key] = *font_size_ptr;
                pm.at("demo_win.tabs_root.tab_files.lbl_font_sample").set(g);
                status("Font path: " + path);
              });
        });

    // ── Forms tab: standalone FileDialog + MessageBox showcase ───────────

    pm.at("demo_win.tabs_root.tab_forms.btn_open_filedialog").onEvent("clicked"_key, [status, browse](dynamic) {
      browse("Open File", "Open", {}, [status](std::string path) { status("FileDialog: opened \"" + path + "\""); });
    });

    // Instantiates a MessageBox with the given title/message/icon/buttons
    // preset and shows the resulting button choice in the status label. The
    // returned proxy is kept alive by capturing it in its own on_result
    // handler -- an RMI proxy with no live reference is destroyed
    // immediately (taking the not-yet-answered dialog down with it), the
    // same pitfall the `browse` lambda above avoids for FileDialog.
    auto show_message_box = [this, status](
                                 const std::string& title, const std::string& message, const std::string& icon,
                                 const std::string& buttons) {
      dynamic params;
      params["title"_key] = title;
      params["message"_key] = message;
      params["icon"_key] = icon;
      params["buttons"_key] = buttons;
      auto raw = this->instantiate("wish"_key, "MessageBox"_key, std::move(params)).get();
      auto mb = std::make_shared<bdg::bison::rmi::proxy::dynamic>(std::move(raw));
      mb->onEvent("on_result"_key, [mb, status](dynamic payload) {
        status("MessageBox result: " + payload.as<std::string>("button"_key));
      });
    };

    pm.at("demo_win.tabs_root.tab_forms.msgbox_row1.btn_mb_ok").onEvent("clicked"_key, [show_message_box](dynamic) {
      show_message_box("Info", "This is a simple OK message box.", "info", "ok");
    });
    pm.at("demo_win.tabs_root.tab_forms.msgbox_row1.btn_mb_ok_cancel")
        .onEvent("clicked"_key, [show_message_box](dynamic) {
          show_message_box("Confirm", "Proceed with the operation?", "question", "ok_cancel");
        });
    pm.at("demo_win.tabs_root.tab_forms.msgbox_row1.btn_mb_yes_no")
        .onEvent("clicked"_key, [show_message_box](dynamic) {
          show_message_box("Question", "Do you want to continue?", "question", "yes_no");
        });
    pm.at("demo_win.tabs_root.tab_forms.msgbox_row2.btn_mb_yes_no_cancel")
        .onEvent("clicked"_key, [show_message_box](dynamic) {
          show_message_box("Confirm", "Save changes before closing?", "warning", "yes_no_cancel");
        });
    pm.at("demo_win.tabs_root.tab_forms.msgbox_row2.btn_mb_retry_cancel")
        .onEvent("clicked"_key, [show_message_box](dynamic) {
          show_message_box("Error", "The operation failed. Retry?", "error", "retry_cancel");
        });
    pm.at("demo_win.tabs_root.tab_forms.msgbox_row2.btn_mb_abort_retry_ignore")
        .onEvent("clicked"_key, [show_message_box](dynamic) {
          show_message_box("Error", "A device error occurred.", "error", "abort_retry_ignore");
        });

    // ── Tab events ────────────────────────────────────────────────────────

    static const char* kTabNames[] = {
        "demo_win.tabs_root.tab_basics",
        "demo_win.tabs_root.tab_sliders",
        "demo_win.tabs_root.tab_inputs",
        "demo_win.tabs_root.tab_selection",
        "demo_win.tabs_root.tab_tree",
        "demo_win.tabs_root.tab_misc",
        "demo_win.tabs_root.tab_tables",
        "demo_win.tabs_root.tab_plots",
        "demo_win.tabs_root.tab_plot3d",
        "demo_win.tabs_root.tab_files",
        "demo_win.tabs_root.tab_forms",
        "demo_win.tabs_root.tab_icons"};
    static const char* kTabLabels[] = {
        "Basics",
        "Sliders & Drags",
        "Text & Numbers",
        "Selection",
        "Tree & Collapse",
        "Misc",
        "Tables",
        "Plots",
        "3-D Plots",
        "Files",
        "Forms",
        "Icons"};
    for (int i = 0; i < 12; ++i) {
      pm.at(kTabNames[i]).onEvent("selected"_key, [i, status](dynamic) {
        status(std::string("Tab selected: ") + kTabLabels[i]);
      });
    }

    vlog("ready - waiting for window close");
    while (!renderer_->should_quit())
      std::this_thread::sleep_for(std::chrono::milliseconds{16});
    vlog("window closed");
  }

 private:
  int click_count_ = 0;
};

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
  // Same-process demo using memory_transport; absolute paths are safe, and the
  // Files tab is what actually exercises the FileDialog/TextEditor/Image paths.
  return wish::examples::run_example(
      argc, argv, "demo", "wish Widget Demo", 900, 800, true,
      [](memory_client_transport t, wish::renderer* r, bool verbose, std::string theme) {
        return std::make_unique<demo_client>(std::move(t), r, verbose, std::move(theme));
      });
}
