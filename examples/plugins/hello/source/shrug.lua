local M = {}

-- ":shrug:" anywhere in the text becomes the shrug emoticon.
function M.apply(text)
	if text:find(":shrug:", 1, true) then
		return (text:gsub(":shrug:", "¯\\_(ツ)_/¯"))
	end
	return nil
end

return M
