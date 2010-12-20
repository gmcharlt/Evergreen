/*
 * Copyright (C) 2010 Internationaal Instituut voor Sociale Geschiedenis <info@iisg.nl>
 * Dan Scott <dan@coffeecode.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.  
 *
 */
dojo.require('dijit.form.Button');
dojo.require('dijit.form.DropDownButton');
dojo.require('dijit.form.FilteringSelect');
dojo.require('dijit.form.Form');
dojo.require('dijit.form.NumberSpinner');
dojo.require('dijit.form.TextBox');
dojo.require("dijit.Menu");
dojo.require("dijit.MenuItem");
dojo.require('dojox.xml.parser');
dojo.require('DojoSRF');
dojo.require("fieldmapper.Fieldmapper");
dojo.require('openils.CGI');
dojo.require('openils.PermaCrud');
dojo.require('openils.XUL');
dojo.require('openils.widget.OrgUnitFilteringSelect');

var cgi = new openils.CGI();

/*
// OrgUnits do not currently affect the retrieval of authority records,
// but this is how to display them if they become OrgUnit-aware
function authOUListInit() {
    new openils.User().buildPermOrgSelector(
        "STAFF_LOGIN", // anywhere you can log in
        dijit.byId("authOU"),
        null, // pre-selected org
        null
    );
}
dojo.addOnLoad(authOUListInit);
*/
function displayAuthorities(data) { 

    var idArr = [];
    // Grab each record from the returned authority records
    dojo.query("record", data).forEach(function(node) {
        var auth = {};
        auth.text = '';
        auth.id = 0;

        // Grab each authority record field from the authority record
        dojo.query("datafield[tag^='1']", node).forEach(function(dfNode) {
            auth.text += dojox.xml.parser.textContent(dfNode); 
            auth.name = dojo.attr(dfNode, 'tag');
            auth.ind1 = dojo.attr(dfNode, 'ind1');
            auth.ind2 = dojo.attr(dfNode, 'ind2');
        });

        
        // Grab the ID of the authority record
        dojo.query("datafield[tag='901'] subfield[code='c']", node).forEach(function(dfNode) {
            auth.id = dojox.xml.parser.textContent(dfNode); 
        });

        idArr.push(parseInt(auth.id));

        // Create the authority record listing entry
        dojo.place('<div class="authEntry" id="auth' + auth.id + '"><span class="text" id="authLabel' + auth.id + '">' + auth.text + '</span></div>', "authlist-div", "last");

        // Add the menu of new/edit/delete/mark-for-merge options
        var auth_menu = new dijit.Menu({});

        // "Edit" menu item
        new dijit.MenuItem({"id": "edit_" + auth.id, "onClick": function(){
            recId = this.id.slice(this.id.lastIndexOf('_') + 1);
            pcrud = new openils.PermaCrud();
            auth_rec = pcrud.retrieve("are", recId);
            if (auth_rec) {
                loadMarcEditor(pcrud, auth_rec);
            }
        }, "label":"Edit"}).placeAt(auth_menu, "first");

        // "Merge" menu item
        new dijit.MenuItem({"id": "merge_" + auth.id, "onClick":function(){
            auth.text = '';
            recId = this.id.slice(this.id.lastIndexOf('_') + 1);
            dojo.query('#auth' + recId + ' span.text').forEach(function(node) {
                auth.text += dojox.xml.parser.textContent(node); 
            });

            // If there is a toMerge item already, this is a target record
            var mergeRole = '<td style="border: 1px solid black; padding-left: 0.5em; padding-right: 1em;">';
            var isTarget = dojo.query('.toMerge').length;
            if (isTarget) {
                mergeRole += 'Target</td>';
            } else {
                mergeRole += 'Master</td>';
            }

            dojo.place('<tr class="toMerge" id="toMerge_' + recId + '"><td>' + mergeRole + '</td><td  style="border: 1px solid black;" id="mergeMeta_' + recId + '"></td><td style="border: 1px solid black; padding-left: 1em; padding-right: 1em;" >' + auth.text + '</td></tr>', 'mergebox-tbody', 'last');
            dojo.place('<span class="authmeta" style="font-family: monospace;">' + auth.name + ' ' + auth.ind1 + auth.ind2 + '</span>', 'mergeMeta_' + recId, 'last');
            dojo.removeClass('mergebox-div', 'hidden');
        }, "label":"Mark for Merge"}).placeAt(auth_menu, "last");

        // "Delete" menu item
        new dijit.MenuItem({"id": "delete_" + auth.id, "onClick":function(){
            recId = this.id.slice(this.id.lastIndexOf('_') + 1);

            // Deleting an authority record is unusual; let's be 100% sure
            if (!confirm("Are you sure you want to delete record " + recId + "?")) {
                return;
            }

            pcrud = new openils.PermaCrud();
            auth_rec = pcrud.retrieve("are", recId);
            if (auth_rec) {
                pcrud.eliminate(auth_rec);
                alert("Deleted authority record # " + recId);
            }
        }, "label":"Delete"}).placeAt(auth_menu, "last");

        auth_mb = new dijit.form.DropDownButton({dropDown: auth_menu, label:"Actions", id:"menu" + auth.id});
        auth_mb.placeAt("auth" + auth.id, "first");
        auth_menu.startup();
    });

    showBibCount(idArr);

}

function showBibCount(authIds) {
    /* Decorate the list with # of bibs linked to each authority record */
    var ses = new OpenSRF.ClientSession('open-ils.cat');
    var req = ses.request('open-ils.cat.authority.records.count_linked_bibs', authIds);
    var linkedIds = [];
    req.oncomplete = function(r) {
        var msg = r.recv().content();
        dojo.forEach(msg, function(auth) {
                linkedIds.push(auth.authority);
                dojo.place('<span class="bibcount">' + auth.bibs + '</span>', 'authLabel' + auth.authority, 'before');
            }
        );

        /* Assign counts of 0 for every non-linked authority */
        dojo.forEach(authIds, function (id) {
            var found = false;
            dojo.forEach(linkedIds, function (lid) {
                if (id == lid) {
                    found = true;
                }
            });
            if (!found) {
                dojo.place('<span class="bibcount">0</span>', 'authLabel' + id, 'before');
            }
        });
    }
    req.send();
}

function loadMarcEditor(pcrud, rec) {
    /*
       To run in Firefox directly, must set signed.applets.codebase_principal_support
       to true in about:config
     */
    netscape.security.PrivilegeManager.enablePrivilege('UniversalXPConnect');
    win = window.open('/xul/server/cat/marcedit.xul'); // XXX version?

    win.xulG = {
        "record": {"marc": rec.marc(), "rtype": "are"},
        "save": {
            "label": "Save",
            "func": function(xmlString) {
                rec.marc(xmlString);
                rec.edit_date('now');
                rec.ischanged(true);
                pcrud.update(rec);
                alert("Record was saved");
                win.close();
            }
        }
    };
}

function authListInit() {
    var term = cgi.param('authTerm') || '';
    var page = cgi.param('authPage') || 0;
    var axis = cgi.param('authAxis') || 'authority.author';
    if (axis) {
        dijit.byId('authAxis').attr('value', axis);
    }
    if (page) {
        dijit.byId('authPage').attr('value', page);
    }
    if (term) {
        dijit.byId('authTerm').attr('value', term);
        displayRecords();
    }

    dojo.connect(dijit.byId('authAxis'), 'onKeyPress', function(evt) {
        if (evt.keyCode == dojo.keys.ENTER) {
            dijit.byId('authPage').attr('value', 0);
            displayRecords();
        }
    }); 

    dojo.connect(dijit.byId('authPage'), 'onKeyPress', function(evt) {
        if (evt.keyCode == dojo.keys.ENTER) {
            dijit.byId('authPage').attr('value', 0);
            displayRecords();
        }
    });

    dojo.connect(dijit.byId('authTerm'), 'onKeyPress', function(evt) {
        if (evt.keyCode == dojo.keys.ENTER) {
            dijit.byId('authPage').attr('value', 0);
            displayRecords();
        }
    });

    dijit.byId('authTerm').focus();

}
dojo.addOnLoad(authListInit);

function displayRecords(parms) {

    if (parms && parms.page) {
        if (parms.page == 'next') {
            page = dijit.byId('authPage').attr('value');
            dijit.byId('authPage').attr('value', page + 1);
        } else if (parms.page == 'prev') {
            page = dijit.byId('authPage').attr('value');
            dijit.byId('authPage').attr('value', page - 1);
        } else {
            dijit.byId('authPage').attr('value', parms.page);
        }
    }

    /* Protect against null input */
    if (!dijit.byId('authTerm').attr('value')) {
        return;
    }

    /* Clear out the current contents of the page */
    var widgets = dijit.findWidgets(dojo.byId('authlist-div'));
    dojo.forEach(widgets, function(w) { w.destroyRecursive(true); });

    dojo.query("#authlist-div div").orphan();

    var url = '/opac/extras/startwith/marcxml/'
        + dijit.byId('authAxis').attr('value')
        // + '/' + dijit.byId('authOU').attr('value')
        + '/1' // replace with preceding line if OUs gain some meaning
        + '/' + dijit.byId('authTerm').attr('value')
        + '/' + dijit.byId('authPage').attr('value')
        + '/' + '20' // 20 results per page
    ;
    dojo.xhrGet({"url":url, "handleAs":"xml", "content":{"format":"marcxml"}, "preventCache": true, "load":displayAuthorities });
}

function clearMergeRecords() {
    var records = dojo.query('.toMerge').orphan();
    dojo.addClass('mergebox-div', 'hidden');
}

function mergeRecords() {
    var records = dojo.query('.toMerge').attr('id');
    dojo.forEach(records, function(item, idx) {
        records[idx] = parseInt(item.slice(item.lastIndexOf('_') + 1));
    });

    /* Take the first record in the list and use that as the master */
    fieldmapper.standardRequest(
        ['open-ils.cat', 'open-ils.cat.authority.records.merge'],
        {   async: false,
            params: [openils.User.authtoken, records.shift(), records],
            oncomplete : function(r) {
                alert("Record merge is complete.");
                clearMergeRecords();
                displayRecords();
            }
        }
    );
}