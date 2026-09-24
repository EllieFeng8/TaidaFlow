Superseded (w2-037 v1): first implementation that wrote a NEW 'Core status 正常' row on DI clear.
PM design change (Mango, 2026-09-24, received mid-task): update the original row in place instead. These files document the v1 build/run only; they are NOT the evidence for the final DoD.
v1 run A left rows id=54..65 in build/runtime-cwd/data/sensor_202609.sqlite (6 raise rows 異常 + 6 clear rows 正常 '... 解除（DIx=y）'). They were not deleted.
