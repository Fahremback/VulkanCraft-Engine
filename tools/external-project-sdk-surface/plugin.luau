local M = {}

function M.register(api)
  return api:register_type("example.external.luau", {
    version = "1.0.0",
    capabilities = { "events", "nodes" }
  })
end

return M
