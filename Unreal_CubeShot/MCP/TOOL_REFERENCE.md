# CubeShot MCP tool reference

API `1.2.0` · provider `cubeshot` · 9 tools

Generated from an actual Editor `tools/list` response. Edit the tool schema, then regenerate this file.

| Tool | Required arguments |
| --- | --- |
| `cubeshot_status` | None |
| `cubeshot_find_meshes` | `query` |
| `cubeshot_preflight` | None |
| `cubeshot_generate` | `mesh_path` |
| `cubeshot_export_png` | `texture_path` |
| `cubeshot_profiles` | `action` |
| `cubeshot_job_start` | `request_id`, `mesh_paths` |
| `cubeshot_job_status` | exactly one of `job_id` / `request_id` |
| `cubeshot_job_control` | `action`; exactly one of `job_id` / `request_id` |

## cubeshot_status

Read CubeShot capture availability, current project and output policy. Check this before capture; it never starts PIE or changes a level.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {},
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_find_meshes

Search the current project's StaticMesh assets by name. Returns exact object paths without loading meshes. Narrow query/root when truncated.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "limit": {
      "default": 40,
      "maximum": 200,
      "minimum": 1,
      "type": "integer"
    },
    "query": {
      "minLength": 1,
      "type": "string"
    },
    "root": {
      "default": "/Game",
      "type": "string"
    }
  },
  "required": [
    "query"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_preflight

Validate one mesh_path or a mesh_paths batch without generating assets. Returns six Texture2D paths, selected PNG paths, errors, warnings and overwrite requirements. Supports all capture options and registered mounts.

### Input schema

```json
{
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "mesh_path"
      ]
    },
    {
      "required": [
        "mesh_paths"
      ]
    }
  ],
  "properties": {
    "auto_fit": {
      "default": true,
      "type": "boolean"
    },
    "background": {
      "default": "transparent",
      "enum": [
        "transparent",
        "solid",
        "texture"
      ],
      "type": "string"
    },
    "background_texture": {
      "type": "string"
    },
    "camera_distance": {
      "maximum": 10000000,
      "minimum": 0.01,
      "type": "number"
    },
    "export_png": {
      "default": false,
      "type": "boolean"
    },
    "field_of_view": {
      "maximum": 170,
      "minimum": 1,
      "type": "number"
    },
    "mesh_path": {
      "type": "string"
    },
    "mesh_paths": {
      "items": {
        "type": "string"
      },
      "minItems": 1,
      "type": "array",
      "uniqueItems": true
    },
    "ortho_width": {
      "maximum": 10000000,
      "minimum": 0.01,
      "type": "number"
    },
    "padding_pixels": {
      "default": 8,
      "maximum": 64,
      "minimum": 0,
      "type": "integer"
    },
    "png_directions": {
      "items": {
        "enum": [
          "Front",
          "Back",
          "Left",
          "Right",
          "Top",
          "Bottom"
        ],
        "type": "string"
      },
      "maxItems": 6,
      "minItems": 1,
      "type": "array",
      "uniqueItems": true
    },
    "projection": {
      "default": "orthographic",
      "enum": [
        "orthographic",
        "perspective"
      ],
      "type": "string"
    },
    "quarter_turns": {
      "default": 0,
      "maximum": 3,
      "minimum": 0,
      "type": "integer"
    },
    "resolution": {
      "default": 512,
      "maximum": 4096,
      "minimum": 256,
      "type": "integer"
    },
    "solid_color": {
      "items": {
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "maxItems": 4,
      "minItems": 4,
      "type": "array"
    },
    "trim_transparent": {
      "default": true,
      "type": "boolean"
    }
  },
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_generate

Capture and save six Texture2D views. The asynchronous MCP request completes after the staged operation; direct Run executes the same steps inline. Requires rendering Editor with PIE/SIE stopped. Defaults remain 512/orthographic/transparent. Existing assets retain identity. Partial saves report dirty packages and staging.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "auto_fit": {
      "default": true,
      "type": "boolean"
    },
    "background": {
      "default": "transparent",
      "enum": [
        "transparent",
        "solid",
        "texture"
      ],
      "type": "string"
    },
    "background_texture": {
      "type": "string"
    },
    "camera_distance": {
      "maximum": 10000000,
      "minimum": 0.01,
      "type": "number"
    },
    "export_png": {
      "default": false,
      "type": "boolean"
    },
    "field_of_view": {
      "maximum": 170,
      "minimum": 1,
      "type": "number"
    },
    "mesh_path": {
      "type": "string"
    },
    "ortho_width": {
      "maximum": 10000000,
      "minimum": 0.01,
      "type": "number"
    },
    "overwrite": {
      "default": false,
      "type": "boolean"
    },
    "padding_pixels": {
      "default": 8,
      "maximum": 64,
      "minimum": 0,
      "type": "integer"
    },
    "png_directions": {
      "items": {
        "enum": [
          "Front",
          "Back",
          "Left",
          "Right",
          "Top",
          "Bottom"
        ],
        "type": "string"
      },
      "maxItems": 6,
      "minItems": 1,
      "type": "array",
      "uniqueItems": true
    },
    "projection": {
      "default": "orthographic",
      "enum": [
        "orthographic",
        "perspective"
      ],
      "type": "string"
    },
    "quarter_turns": {
      "default": 0,
      "maximum": 3,
      "minimum": 0,
      "type": "integer"
    },
    "resolution": {
      "default": 512,
      "maximum": 4096,
      "minimum": 256,
      "type": "integer"
    },
    "solid_color": {
      "items": {
        "maximum": 1,
        "minimum": 0,
        "type": "number"
      },
      "maxItems": 4,
      "minItems": 4,
      "type": "array"
    },
    "trim_transparent": {
      "default": true,
      "type": "boolean"
    }
  },
  "required": [
    "mesh_path"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_export_png

Export a generated CubeShot texture to Saved/CubeShot/MCPExports. Optionally trim transparent margins with padding and rotate clockwise in 90-degree steps for item-slot framing. Captured color/alpha pixels are preserved without resampling; the source texture is unchanged. overwrite defaults true for legacy compatibility; false protects existing files. existing_file_observed is advisory and does not prove a concurrent replacement. Returns dimensions, applied options and alpha coverage.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "overwrite": {
      "default": true,
      "type": "boolean"
    },
    "padding_pixels": {
      "default": 8,
      "maximum": 64,
      "minimum": 0,
      "type": "integer"
    },
    "quarter_turns": {
      "default": 0,
      "maximum": 3,
      "minimum": 0,
      "type": "integer"
    },
    "texture_path": {
      "type": "string"
    },
    "trim_transparent": {
      "default": false,
      "type": "boolean"
    }
  },
  "required": [
    "texture_path"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_profiles

List/get/save/delete CubeShot profiles. Builtins editor_default and item_icon are immutable. Save takes a versioned provider/id/display_name/options profile JSON; overwrite approvals are never stored.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "action": {
      "enum": [
        "list",
        "get",
        "save",
        "delete"
      ],
      "type": "string"
    },
    "id": {
      "type": "string"
    },
    "overwrite": {
      "default": false,
      "type": "boolean"
    },
    "profile": {
      "type": "object"
    }
  },
  "required": [
    "action"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_job_start

Enqueue an ordered CubeShot batch with a durable request_id. Same ID and same request returns the original job; changed request is rejected. Options priority: overrides, common options, profile, API defaults. Admission performs no mesh loads. Overwrite defaults false for Texture2D and PNG.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "mesh_paths": {
      "items": {
        "type": "string"
      },
      "minItems": 1,
      "type": "array",
      "uniqueItems": true
    },
    "options": {
      "additionalProperties": false,
      "properties": {
        "auto_fit": {
          "default": true,
          "type": "boolean"
        },
        "background": {
          "default": "transparent",
          "enum": [
            "transparent",
            "solid",
            "texture"
          ],
          "type": "string"
        },
        "background_texture": {
          "type": "string"
        },
        "camera_distance": {
          "maximum": 10000000,
          "minimum": 0.01,
          "type": "number"
        },
        "export_png": {
          "default": false,
          "type": "boolean"
        },
        "field_of_view": {
          "maximum": 170,
          "minimum": 1,
          "type": "number"
        },
        "ortho_width": {
          "maximum": 10000000,
          "minimum": 0.01,
          "type": "number"
        },
        "padding_pixels": {
          "default": 8,
          "maximum": 64,
          "minimum": 0,
          "type": "integer"
        },
        "png_directions": {
          "items": {
            "enum": [
              "Front",
              "Back",
              "Left",
              "Right",
              "Top",
              "Bottom"
            ],
            "type": "string"
          },
          "maxItems": 6,
          "minItems": 1,
          "type": "array",
          "uniqueItems": true
        },
        "projection": {
          "default": "orthographic",
          "enum": [
            "orthographic",
            "perspective"
          ],
          "type": "string"
        },
        "quarter_turns": {
          "default": 0,
          "maximum": 3,
          "minimum": 0,
          "type": "integer"
        },
        "resolution": {
          "default": 512,
          "maximum": 4096,
          "minimum": 256,
          "type": "integer"
        },
        "solid_color": {
          "items": {
            "maximum": 1,
            "minimum": 0,
            "type": "number"
          },
          "maxItems": 4,
          "minItems": 4,
          "type": "array"
        },
        "trim_transparent": {
          "default": true,
          "type": "boolean"
        }
      },
      "type": "object"
    },
    "overrides": {
      "additionalProperties": {
        "additionalProperties": false,
        "properties": {
          "auto_fit": {
            "default": true,
            "type": "boolean"
          },
          "background": {
            "default": "transparent",
            "enum": [
              "transparent",
              "solid",
              "texture"
            ],
            "type": "string"
          },
          "background_texture": {
            "type": "string"
          },
          "camera_distance": {
            "maximum": 10000000,
            "minimum": 0.01,
            "type": "number"
          },
          "export_png": {
            "default": false,
            "type": "boolean"
          },
          "field_of_view": {
            "maximum": 170,
            "minimum": 1,
            "type": "number"
          },
          "ortho_width": {
            "maximum": 10000000,
            "minimum": 0.01,
            "type": "number"
          },
          "padding_pixels": {
            "default": 8,
            "maximum": 64,
            "minimum": 0,
            "type": "integer"
          },
          "png_directions": {
            "items": {
              "enum": [
                "Front",
                "Back",
                "Left",
                "Right",
                "Top",
                "Bottom"
              ],
              "type": "string"
            },
            "maxItems": 6,
            "minItems": 1,
            "type": "array",
            "uniqueItems": true
          },
          "projection": {
            "default": "orthographic",
            "enum": [
              "orthographic",
              "perspective"
            ],
            "type": "string"
          },
          "quarter_turns": {
            "default": 0,
            "maximum": 3,
            "minimum": 0,
            "type": "integer"
          },
          "resolution": {
            "default": 512,
            "maximum": 4096,
            "minimum": 256,
            "type": "integer"
          },
          "solid_color": {
            "items": {
              "maximum": 1,
              "minimum": 0,
              "type": "number"
            },
            "maxItems": 4,
            "minItems": 4,
            "type": "array"
          },
          "trim_transparent": {
            "default": true,
            "type": "boolean"
          }
        },
        "type": "object"
      },
      "type": "object"
    },
    "overwrite": {
      "default": false,
      "type": "boolean"
    },
    "profile_id": {
      "type": "string"
    },
    "request_id": {
      "type": "string"
    }
  },
  "required": [
    "request_id",
    "mesh_paths"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_job_status

Read a persisted job by job_id or request_id. success denotes successful lookup; state denotes actual execution outcome. Reports ordered items, progress, output and recovery paths.

Identify the job with exactly one of `job_id` and `request_id`; supplying both or neither is rejected.

### Input schema

```json
{
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "job_id"
      ]
    },
    {
      "required": [
        "request_id"
      ]
    }
  ],
  "properties": {
    "job_id": {
      "minLength": 1,
      "type": "string"
    },
    "request_id": {
      "minLength": 1,
      "type": "string"
    }
  },
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```

## cubeshot_job_control

Request cancel at a safe precommit boundary or acknowledge_recovery to release reviewed output reservations. Acknowledgement does not restore/delete files. Cancellation acceptance differs from application.

Identify the job with exactly one of `job_id` and `request_id`; supplying both or neither is rejected.

### Input schema

```json
{
  "additionalProperties": false,
  "oneOf": [
    {
      "required": [
        "job_id"
      ]
    },
    {
      "required": [
        "request_id"
      ]
    }
  ],
  "properties": {
    "action": {
      "enum": [
        "cancel",
        "acknowledge_recovery"
      ],
      "type": "string"
    },
    "job_id": {
      "minLength": 1,
      "type": "string"
    },
    "request_id": {
      "minLength": 1,
      "type": "string"
    }
  },
  "required": [
    "action"
  ],
  "type": "object"
}
```

### Output schema

```json
{
  "additionalProperties": true,
  "oneOf": [
    {
      "properties": {
        "success": {
          "const": true
        }
      }
    },
    {
      "properties": {
        "error": {
          "type": "string"
        },
        "error_code": {
          "type": "string"
        },
        "success": {
          "const": false
        }
      },
      "required": [
        "error"
      ]
    }
  ],
  "properties": {
    "success": {
      "type": "boolean"
    }
  },
  "required": [
    "success"
  ],
  "type": "object"
}
```
