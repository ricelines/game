extends RefCounted
## Pure formatting of recorded causal identities and attribution.


static func format(records: Dictionary, trace_root: int, thing: String = "") -> String:
	var ids = records.keys()
	ids.sort()
	ids.reverse()
	var text = "CAUSAL CHAIN #%d\n\n" % trace_root if trace_root > 0 else ""
	if trace_root == 0 and not thing.is_empty():
		text = "OBJECT RECORD %s\n\n" % thing
	var count = 0
	for id in ids:
		var event = records[id]
		if trace_root == 0 and not thing.is_empty() and not thing in event.get("things", []):
			continue
		if (
			trace_root == 0
			and thing.is_empty()
			and event.get("kind", "") == "handler"
			and not event.get("changed", false)
		):
			continue
		if trace_root > 0 and int(event.get("root", 0)) != trace_root:
			continue
		text += (
			"%s   [url=%s]#%s[/url]\n%s\n%s · %s\n"
			% [
				str(event.get("kind", "event")).to_upper(),
				id,
				id,
				event.get("name", ""),
				event.get("actor", ""),
				event.get("result", "")
			]
		)
		var cause = "—" if event.get("cause") == null else "#%d" % int(event.cause)
		text += "↳ root #%d · cause %s\n" % [int(event.get("root", id)), cause]
		var authors: Array[String] = []
		for author in event.get("authors", []):
			authors.append(str(author))
		text += "authors: %s\n" % (", ".join(authors) if not authors.is_empty() else "—")
		text += "\n"
		count += 1
		if count >= 35:
			break
	return text if not text.is_empty() else "The record is waiting for events."


static func link_layout(label: RichTextLabel) -> Array:
	var result = []
	var text = label.get_parsed_text()
	var origin = label.get_global_rect().position
	for line in label.get_line_count():
		var span = label.get_line_range(line)
		var parts = text.substr(span.x, span.y - span.x).strip_edges().split("   #")
		if parts.size() == 2 and parts[1].is_valid_int():
			result.append(
				{
					"id": int(parts[1]),
					"point":
					[
						origin.x + label.get_line_width(line) * 0.8,
						origin.y + label.get_line_offset(line) + label.get_line_height(line) * 0.5
					]
				}
			)
	return result
