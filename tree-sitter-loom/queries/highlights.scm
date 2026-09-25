"func" @keyword.function
"return" @keyword.return
"import" @keyword.import
["export" "extern"] @keyword.storage
"@entity" @attribute
["public" "private" "static"] @keyword.storage
["let" "const" "enum" "struct" "class" "type" "data" "namespace"] @keyword.storage
["virtual" "override"] @keyword.storage
"operator" @keyword.storage
(class_definition "extends" @keyword.storage)
(struct_operator_method operator: _ @operator)
(class_operator_method operator: _ @operator)
(function_call name: (namespaced_identifier) @keyword.function (#eq? @keyword.function "super"))
(data_get_expression ["storage" "entity" "block"] @keyword.conditional)
(data_set_statement ["storage" "entity" "block"] @keyword.conditional)
(data_get_expression target: (resource_location) @string)
(data_set_statement target: (resource_location) @string)
(data_get_expression path: (nbt_path) @string.special)
(data_set_statement path: (nbt_path) @string.special)
(map_type "map" @type.builtin)
(map_expression "map" @type.builtin)
["if" "else"] @keyword.conditional
["while" "do" "for"] @keyword.repeat
(for "in" @keyword.repeat)
(import_statement "as" @keyword.import)
(import_statement alias: (identifier) @module)
(namespace_definition name: (identifier) @module)

["as" "at" "align" "anchored" "facing" "on" "positioned" "rotated"] @keyword.conditional
(contextModifier "in" @keyword.conditional)
("facing" "entity" @keyword.conditional)
("positioned" [ "as" "over" ] @keyword.conditional)
("rotated" "as" @keyword.conditional)

["eyes" "feet"] @constant.builtin
["attacker" "controller" "leasher" "origin" "owner" "passengers" "target" "vehicle"] @constant.builtin
["world_surface" "motion_blocking" "motion_blocking_no_leaves" "ocean_floor"] @constant.builtin

"#" @attribute
(function_definition tag: (namespaced_arg) @attribute)
(contextModifier dim: (namespaced_arg) @constant)

(selector [ "@s" "@r" "@p" "@e" "@a" "@n" ]) @variable.builtin
(selector) @variable.parameter

(struct_definition name: (identifier) @type)
(struct_field name: (identifier) @property)
(struct_method name: (identifier) @method)
(class_definition name: (identifier) @type)
(class_definition parent: (identifier) @type)
(class_method name: (identifier) @method)
(enum_definition name: (identifier) @type)
(type_alias_definition name: (identifier) @type)
(enum_variant name: (identifier) @constant)

(property_access property: (identifier) @property)
(member_expression property: (identifier) @property)
(method_call_expression method: (identifier) @method.call)

(member_expression
  object: (variable_ref name: (namespaced_identifier) @type)
  property: (identifier) @constant
  (#match? @type "^[A-Z]")
  (#match? @constant "^[A-Z]"))

(function_definition name: (identifier) @function)
(function_call name: (namespaced_identifier) @function.call)
(variable_declaration name: (identifier) @variable)
(parameter name: (identifier) @variable.parameter)
(assignment name: (namespaced_identifier) @variable)
(for iterator: (identifier) @variable)
(variable_ref name: (namespaced_identifier) @variable.reference)

(namespaced_identifier) @module

(command_name) @function.builtin
(command_arg) @string
(string_literal) @string

(swizzle) @type
(integer) @number
(float) @number
(boolean) @boolean
(vec2) @number
(vec3) @number

".." @operator
"=" @operator
"::" @punctuation.delimiter
(unary_expression operator: _ @operator)
(binary_expression operator: _ @operator)
(struct_expression name: (namespaced_identifier) @type)
(struct_expression_field name: (identifier) @property)
[":" ";" "(" ")" "{" "}" "," "[" "]" "[]" "!"] @punctuation.delimiter

(comment) @comment @spell

(interpolation ["${" "}"] @punctuation.special)

(function_call name: (namespaced_identifier) @function.builtin (#match? @function.builtin "^(append|remove|insert|len|contains)$"))
