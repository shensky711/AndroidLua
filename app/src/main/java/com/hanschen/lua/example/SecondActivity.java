package com.hanschen.lua.example;

import android.os.Bundle;
import android.support.annotation.Nullable;
import android.support.v7.app.AppCompatActivity;
import android.util.Log;

import org.keplerproject.luajava.JavaFunction;
import org.keplerproject.luajava.LuaException;
import org.keplerproject.luajava.LuaState;
import org.keplerproject.luajava.LuaStateFactory;

/**
 * Created by chenhang on 2016/9/30.
 */

public class SecondActivity extends AppCompatActivity {

    private LuaState L;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
    }

    private void initLua() {
        L = LuaStateFactory.newLuaState();
        L.openLibs();

        try {
            L.pushJavaObject(this);
            L.setGlobal("activity");

            JavaFunction print = new JavaFunction(L) {
                @Override
                public int execute() throws LuaException {
                    StringBuilder stringBuilder = new StringBuilder();
                    stringBuilder.append("[").append(System.currentTimeMillis()).append("] ");
                    for (int i = 2; i <= L.getTop(); i++) {
                        int type = L.type(i);
                        String stype = L.typeName(type);
                        String val = null;
                        if (stype.equals("userdata")) {
                            Object obj = L.toJavaObject(i);
                            if (obj != null)
                                val = obj.toString();
                        } else if (stype.equals("boolean")) {
                            val = L.toBoolean(i) ? "true" : "false";
                        } else {
                            val = L.toString(i);
                        }
                        if (val == null)
                            val = stype;
                        stringBuilder.append(val).append("\t");
                    }
                    stringBuilder.append("\n");
                    Log.d("Hans", stringBuilder.toString());
                    return 0;
                }
            };
            print.register("print");
        } catch (Exception e) {
        }
    }
}
