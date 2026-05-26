#target photoshop
app.bringToFront();

(function () {
  // ---------- utils ----------
  function selectFolder(prompt){ var f = Folder.selectDialog(prompt); if(!f) throw("Canceled"); return f; }
  function listPngs(folder){
    var files = folder.getFiles(function(f){ return f instanceof File && /\.png$/i.test(f.name); });
    files.sort(function(a,b){ return decodeURI(a.name).toLowerCase() < decodeURI(b.name).toLowerCase() ? -1 : 1; });
    if(files.length===0) throw("No PNGs in folder");
    return files;
  }
  function asPx(v){ return Number(v.as("px")); }
  function boundsCenterPx(b){ return { cx: (asPx(b[0])+asPx(b[2]))*0.5, cy:(asPx(b[1])+asPx(b[3]))*0.5 }; }
  function moveLayerCenterTo(L, cx, cy){
    var c = boundsCenterPx(L.bounds);
    L.translate(cx - c.cx, cy - c.cy);
  }
  function fillAll(doc, color){
    var sel = doc.selection;
    sel.selectAll();
    var solid = new SolidColor(); solid.rgb.red = color[0]; solid.rgb.green = color[1]; solid.rgb.blue = color[2];
    sel.fill(solid, ColorBlendMode.NORMAL, 100, false);
    sel.deselect();
  }

  var prevUnits = app.preferences.rulerUnits;
  app.preferences.rulerUnits = Units.PIXELS;

  // ---------- выбор и размеры кадра ----------
  var folder = selectFolder("Выбери папку с PNG-кадрами");
  var files = listPngs(folder);

  var probe = app.open(files[0]); // размеры РАМКИ кадра
  var frameW = probe.width.as("px");
  var frameH = probe.height.as("px");
  probe.close(SaveOptions.DONOTSAVECHANGES);

  // ---------- диалог ----------
  var vertical = true;
  var spacing = 0;
  (function(){
    var d = new Window("dialog","Filmstrip (через группы и фон)");
    d.orientation = "column"; d.alignChildren = "left";

    var g1 = d.add("group"); g1.orientation = "column";
    g1.add("statictext", undefined, "Ориентация:");
    var rbV = g1.add("radiobutton", undefined, "Вертикально (1 × N)");
    var rbH = g1.add("radiobutton", undefined, "Горизонтально (N × 1)");
    rbV.value = true;

    var g2 = d.add("group");
    g2.add("statictext", undefined, "Отступ между кадрами (px):");
    var ed = g2.add("edittext", undefined, "0"); ed.characters = 6;

    var g3 = d.add("group"); g3.alignment = "right";
    g3.add("button", undefined, "OK", {name:"ok"});
    g3.add("button", undefined, "Отмена", {name:"cancel"});

    if (d.show() !== 1) throw("Canceled");
    vertical = rbV.value;
    spacing  = Math.max(0, parseInt(ed.text,10)||0);
  })();

  // ---------- создаём итоговый документ ----------
  var count = files.length;
  var docW = vertical ? frameW : frameW*count + spacing*(count-1);
  var docH = vertical ? frameH*count + spacing*(count-1) : frameH;

  var dest = app.documents.add(UnitValue(docW,"px"), UnitValue(docH,"px"), 72, "filmstrip_groups",
                               NewDocumentMode.RGB, DocumentFill.TRANSPARENT);

  // центры ячеек: у первого кадра сдвиг + frameH/2 по Y (или + frameW/2 по X)
  var cellCenters = [];
  if (vertical) {
    var y0 = frameH * 0.5; // «+ половина высоты документа кадра»
    for (var i=0;i<count;i++) cellCenters.push({ cx: frameW*0.5, cy: y0 + i*(frameH + spacing) });
  } else {
    var x0 = frameW * 0.5;
    for (var j=0;j<count;j++) cellCenters.push({ cx: x0 + j*(frameW + spacing), cy: frameH*0.5 });
  }

  // ---------- основной цикл ----------
  for (var k=0; k<count; ++k){
    var src = app.open(files[k]);

    // 1) группа-«пакет»
    var grp = src.layerSets.add(); grp.name = "__pack__";

    // 2) белая подложка во весь холст (внутрь группы)
    var pad = src.artLayers.add(); pad.name = "__pad__";
    pad.move(grp, ElementPlacement.INSIDE);
    src.activeLayer = pad;
    fillAll(src, [255,255,255]); // 100% белый фон ровно по размеру документа

    // 3) перенести ВСЕ прочие верхнеуровневые слои внутрь группы
    //    (снимем снапшот списка, чтобы перемещение не ломало порядок обхода)
    var topLayers = [];
    for (var t=0; t<src.layers.length; ++t) topLayers.push(src.layers[t]);
    for (var m=0; m<topLayers.length; ++m){
      var L = topLayers[m];
      if (L !== grp) { L.move(grp, ElementPlacement.INSIDE); }
    }
    // подложка — в самый низ группы
    pad.move(grp, ElementPlacement.PLACEATEND);

    // 4) дублируем группу в целевой документ
    var dup = grp.duplicate(dest, ElementPlacement.PLACEATEND);

    // исходник нам больше не нужен
    src.close(SaveOptions.DONOTSAVECHANGES);

    // 5) позиционируем дублированную группу по центру своей ячейки
    app.activeDocument = dest;
    moveLayerCenterTo(dup, cellCenters[k].cx, cellCenters[k].cy);

    // 6) удаляем подложку внутри дублированной группы
    // (ищем слой по имени __pad__ только ВНУТРИ этой группы)
    (function removePadInside(set){
      // среди artLayers группы
      for (var a=set.artLayers.length-1; a>=0; --a){
        if (set.artLayers[a].name === "__pad__") { set.artLayers[a].remove(); return; }
      }
      // если вдруг подслои/вложенные группы — обойдём рекурсивно
      for (var s=0; s<set.layerSets.length; ++s) removePadInside(set.layerSets[s]);
    })(dup);
  }

  // ---------- экспорт ----------
  var outPng = File(folder.fsName + "/filmstrip_" + (vertical ? "vertical" : "horizontal") + "_groups.png");
  var web = new ExportOptionsSaveForWeb();
  web.format = SaveDocumentType.PNG; web.PNG8=false; web.transparency=true; web.interlaced=false;
  dest.exportDocument(outPng, ExportType.SAVEFORWEB, web);

  /* var outPsd = File(folder.fsName + "/filmstrip_" + (vertical ? "vertical" : "horizontal") + "_groups.psd");
  var psd = new PhotoshopSaveOptions(); psd.layers = true;
  dest.saveAs(outPsd, psd, true, Extension.LOWERCASE); */

  app.preferences.rulerUnits = prevUnits;

  alert("Готово:\n" + outPng.fsName); // + "\n" + outPsd.fsName
})();
