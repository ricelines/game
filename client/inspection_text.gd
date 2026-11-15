extends RefCounted
## Selected-object presentation; maker, elaboration and measured facts stay separate.


static func elaborated(inspection: Dictionary) -> String:
	var text = ""
	var elaborated = inspection.get("elaborated", [])
	if not elaborated.is_empty():
		text += "\n\nELABORATED DETAILS\n"
		for detail in elaborated:
			text += (
				"%s\n%s · at %.1f s · detail #%s\n"
				% [
					str(detail.get("text", "")),
					str(detail.get("author", "")),
					float(detail.get("created_s", 0)),
					str(detail.get("id", 0))
				]
			)
	return text


static func world_record(inspection: Dictionary, debug: bool) -> String:
	var text = ""
	text += "\nWORLD RECORD\n"
	text += (
		"Model  %s\nMade by  %s · at %.1f s\n"
		% [
			str(inspection.get("model", "—")),
			str(inspection.get("maker", "—")),
			float(inspection.get("created_s", 0))
		]
	)
	var material = inspection.get("measured", {}).get("material", {})
	if material is Dictionary and material.has("name"):
		text += "Material  " + str(material.name) + "\n"
	var allowed: Array[String] = []
	for right in inspection.get("rights", {}):
		if inspection.rights[right].get("allowed", false):
			allowed.append(str(right))
	if inspection.has("rights"):
		text += (
			"Your rights  " + (", ".join(allowed) if not allowed.is_empty() else "observe") + "\n"
		)
	if debug:
		text += "\nDEBUG INSPECTION\n" + JSON.stringify(inspection, "  ")
	return text
