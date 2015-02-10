package.path = package.path .. ";lua/?.lua;../?.lua"

local server = require("server")
local replica_set = require("replica_set")

local _M = {
   server_map = {},
   replica_sets = {},

   -- struct server_pool 
   pool = __pool,

   -- resource pools
   _rs_pool = {},
   _se_pool = {},
}

function _M.fetch_server(self, config)
   local s = nil

   if #self._se_pool == 0 then
      s = server:new(config)
   else
      s = table.remove(self._se_pool, 1)
   end
   
   if s:connect() then
      return nil
   end

   return s
end

function _M.put_server(self, s)
   s:disconnect()
   table.insert(self._se_pool, s)
end

function _M.fetch_replica_set(self)
   local rs = nil

   if #self._rs_pool == 0 then
      rs = replica_set:new(self.pool)
   else
      rs = table.remove(self._rs_pool, 1)
   end
   
   return rs
end

function _M.put_replica_set(self, rs)
   rs:deinit()
   table.insert(self._rs_pool, rs)
end

-- Public Methods

function _M.set_servers(self, configs)
   local configs = configs
   local tmp_server_map = {}

   for _, config in ipairs(configs) do
      local id = config.id

      if self.server_map[id] then
         -- Move server to tmp server map
         tmp_server_map[id] = self.server_map[id]
         self.server_map[id] = nil
      else
         -- Fetch server from resource pool
         tmp_server_map[id] = self:fetch_server(config)
      end
   end

   -- Swap server_map
   self.server_map, tmp_server_map = tmp_server_map, self.server_map

   -- Drop servers that we no longer use
   for id, s in pairs(tmp_server_map) do
      self:put_server(s)
   end
end

function _M.build_replica_sets(self)
   local tmp_rss = {}

   -- Set masters
   for id,s in pairs(self.server_map) do
      if s:is_master() then
         local rs = self:fetch_replica_set()
         rs:set_master(s)
         table.insert(tmp_rss, rs)
      end
   end

   -- Set slaves
   for id,s in pairs(self.server_map) do
      if s:is_slave() then
         local ms = self.server_map[s.master_id]
         local rs = ms.replica_set
         rs:add_slave(s)
      end
   end

   -- Swap replicasets
   self.replica_sets, tmp_rss = tmp_rss, self.replica_sets

   -- Recycle
   for _, rs in ipairs(tmp_rss) do
      self:put_replica_set(rs)
   end
end

function _M.bind_slots(self)
   for _,rs in ipairs(self.replica_sets) do
      rs:bind_slots()
   end
end

return _M
