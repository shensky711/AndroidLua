--
-- Created by IntelliJ IDEA.
-- User: chenhang
-- Date: 2016/9/30
-- Time: 16:35
-- To change this template use File | Settings | File Templates.
--

function gc()
    local ret = activity.__gc();
    print('gc status: ' .. ret)
end

