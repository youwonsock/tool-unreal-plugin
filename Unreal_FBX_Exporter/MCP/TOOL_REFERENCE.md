# FBX_Exporter MCP tool reference

API `1.2.0` · provider `fbx_exporter` · 8 tools

Generated from an actual Editor `tools/list` response. Edit the tool schema, then regenerate this file.

| Tool | Required arguments |
| --- | --- |
| `fbx_exporter_status` | None |
| `fbx_exporter_find_meshes` | `query` |
| `fbx_exporter_preflight` | None |
| `fbx_exporter_export` | `mesh_path` |
| `fbx_exporter_profiles` | `action` |
| `fbx_exporter_job_start` | `request_id`, `mesh_paths` |
| `fbx_exporter_job_status` | exactly one of `job_id` / `request_id` |
| `fbx_exporter_job_control` | `action`; exactly one of `job_id` / `request_id` |

## fbx_exporter_status

Read the connected project, FBX export availability, map choices and default output folder. No files are written.

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

## fbx_exporter_find_meshes

Find Static Mesh assets by name in this project or a mounted content root, without loading them. Returns exact object paths. Narrow query/root if truncated; retry if asset_registry_loading is true.

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

## fbx_exporter_preflight

Check mesh LOD0/UV0, renderer and output paths without writing files or opening dialogs. Reports requires_overwrite; replacement includes user files inside the mesh folder. Filesystem permissions and actual bake success are checked during export.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "embed_textures": {
      "default": false,
      "description": "Include PNG bytes inside FBX. External PNGs and material JSON are still produced when maps are selected.",
      "type": "boolean"
    },
    "maps": {
      "description": "Omit for all six maps; [] exports geometry only.",
      "items": {
        "enum": [
          "BaseColor",
          "Normal",
          "Mask",
          "Occlusion",
          "Emission",
          "Opacity"
        ],
        "type": "string"
      },
      "type": "array",
      "uniqueItems": true
    },
    "mesh_path": {
      "description": "Exact Static Mesh object path from fbx_exporter_find_meshes.",
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
    "output_layout": {
      "default": "mesh_name",
      "enum": [
        "mesh_name",
        "asset_path"
      ],
      "type": "string"
    },
    "output_root": {
      "description": "Absolute output root. Default: the current project's Saved/FBX_Exporter/Exports. A mesh-named subfolder is created.",
      "type": "string"
    },
    "resolution": {
      "default": 2048,
      "enum": [
        512,
        1024,
        2048,
        4096
      ],
      "type": "integer"
    },
    "reuse_direct_textures": {
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

## fbx_exporter_export

Export one Static Mesh using FBX_Exporter, with selected PNGs and optional embedded PNG bytes. Requires PIE stopped and a rendering Editor for textures. Run preflight first; existing output is refused unless overwrite=true. Runs to completion on the Editor game thread; allow a long timeout for baking. Returns saved files, warnings and errors. JSON is metadata; FBX stores texture connections.

### Input schema

```json
{
  "additionalProperties": false,
  "properties": {
    "embed_textures": {
      "default": false,
      "description": "Include PNG bytes inside FBX. External PNGs and material JSON are still produced when maps are selected.",
      "type": "boolean"
    },
    "maps": {
      "description": "Omit for all six maps; [] exports geometry only.",
      "items": {
        "enum": [
          "BaseColor",
          "Normal",
          "Mask",
          "Occlusion",
          "Emission",
          "Opacity"
        ],
        "type": "string"
      },
      "type": "array",
      "uniqueItems": true
    },
    "mesh_path": {
      "description": "Exact Static Mesh object path from fbx_exporter_find_meshes.",
      "type": "string"
    },
    "output_layout": {
      "default": "mesh_name",
      "enum": [
        "mesh_name",
        "asset_path"
      ],
      "type": "string"
    },
    "output_root": {
      "description": "Absolute output root. Default: the current project's Saved/FBX_Exporter/Exports. A mesh-named subfolder is created.",
      "type": "string"
    },
    "overwrite": {
      "default": false,
      "description": "Explicitly replace the entire existing mesh output folder, including user files. Inspect preflight first.",
      "type": "boolean"
    },
    "resolution": {
      "default": 2048,
      "enum": [
        512,
        1024,
        2048,
        4096
      ],
      "type": "integer"
    },
    "reuse_direct_textures": {
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

## fbx_exporter_profiles

List/get/save/delete project FBX profiles. Builtin pbr and geometry_only are immutable. Profiles never store overwrite approval.

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

## fbx_exporter_job_start

Persist and queue ordered exports. Returns a job_id before asset loading. Same request_id+request returns the existing job; changed request errors. First failed target stops later targets.

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
        "embed_textures": {
          "default": false,
          "description": "Include PNG bytes inside FBX. External PNGs and material JSON are still produced when maps are selected.",
          "type": "boolean"
        },
        "maps": {
          "description": "Omit for all six maps; [] exports geometry only.",
          "items": {
            "enum": [
              "BaseColor",
              "Normal",
              "Mask",
              "Occlusion",
              "Emission",
              "Opacity"
            ],
            "type": "string"
          },
          "type": "array",
          "uniqueItems": true
        },
        "output_layout": {
          "default": "mesh_name",
          "enum": [
            "mesh_name",
            "asset_path"
          ],
          "type": "string"
        },
        "output_root": {
          "description": "Absolute output root. Default: the current project's Saved/FBX_Exporter/Exports. A mesh-named subfolder is created.",
          "type": "string"
        },
        "resolution": {
          "default": 2048,
          "enum": [
            512,
            1024,
            2048,
            4096
          ],
          "type": "integer"
        },
        "reuse_direct_textures": {
          "default": true,
          "type": "boolean"
        }
      },
      "type": "object"
    },
    "overrides": {
      "additionalProperties": {
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
      "minLength": 1,
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

## fbx_exporter_job_status

Query by job_id or request_id. success describes the lookup, state describes the actual job. Native GPU/FBX calls can delay replies.

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
      "type": "string"
    },
    "request_id": {
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

## fbx_exporter_job_control

Request safe-boundary cancellation or acknowledge inspected recovery without deleting/restoring files. Only the job's own recorded recovery journals can be acknowledged.

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
      "type": "string"
    },
    "request_id": {
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
