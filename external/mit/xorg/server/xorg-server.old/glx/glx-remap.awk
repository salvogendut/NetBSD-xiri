# $NetBSD$

# Generate the old server's extension dispatch table from the Mesa provider
# embedded in libglapi. Both generated headers are inputs to this rule.
BEGIN { bytes = 0 }
/^static const char public_string_pool\[\]/ { pool = 1; next }
pool && /^[[:space:]]*"/ {
	name = $1
	sub(/^"/, "", name)
	sub(/(\\0)?"[;]?$/, "", name)
	names[bytes] = name
	bytes += length(name) + 1
	if ($0 ~ /;$/) pool = 0
	next
}
/^static const struct mapi_stub public_stubs\[\]/ { stubs = 1; next }
stubs && /^};/ { stubs = 0; next }
stubs && /\(void \*\)/ {
	address = $4 + 0
	if (!(address in names)) {
		print "unrecognized Mesa entry point at byte " address > "/dev/stderr"
		exit 1
	}
	slot[names[address]] = $5 + 0
	next
}
/^#define [[:alnum:]_]+_remap_index [0-9]+$/ {
	name = $2
	sub(/_remap_index$/, "", name)
	remap[$3 + 0] = name
	indirect[name] = 1
	count++
	next
}
# The core calls access the table directly using their ABI-defined offsets.
/^#define _gloffset_[[:alnum:]_]+ [0-9]+$/ && FILENAME ~ /glapioffsets\.h$/ {
	name = $2
	sub(/^_gloffset_/, "", name)
	core[name] = $3 + 0
	next
}
# A future server must not refer to a slot its Mesa provider lacks.
FILENAME ~ /\.c$/ {
	line = $0
	while (match(line, /(CALL_|GET_|_gloffset_)[[:alnum:]_]+/)) {
		name = substr(line, RSTART, RLENGTH)
		sub(/^(CALL_|GET_|_gloffset_)/, "", name)
		used[name] = 1
		line = substr(line, RSTART + RLENGTH)
	}
}
END {
	if (!count || !bytes) exit 1
	for (name in core) {
		if (name in indirect || name == "FIRST_DYNAMIC") continue
		if (!(name in slot) || core[name] != slot[name]) {
			print "incompatible core GL offset for " name > "/dev/stderr"
			exit 1
		}
	}
	print "/* Generated from the server and Mesa GL dispatch definitions. */"
	print "int xorgGlxDispatchRemapTable[] = {"
	for (i = 0; i < count; i++) {
		name = remap[i]
		if (name == "" || ((name in used) && !(name in slot))) {
			print "missing Mesa GLX entry for " name > "/dev/stderr"
			exit 1
		}
		printf "\t%d, /* %s */\n", (name in slot) ? slot[name] : -1, name
	}
	print "};"
}
