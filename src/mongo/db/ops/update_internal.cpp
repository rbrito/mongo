//@file update_internal.cpp

/**
 *    Copyright (C) 2012 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"

#include "mongo/db/oplog.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/db/pdfile.h"

#include "update_internal.h"

//#define DEBUGUPDATE(x) cout << x << endl;
#define DEBUGUPDATE(x)

namespace mongo {

    const char* Mod::modNames[] = { "$inc", "$set", "$push", "$pushAll", "$pull", "$pullAll" , "$pop", "$unset" ,
                                    "$bitand" , "$bitor" , "$bit" , "$addToSet", "$rename", "$rename"
                                  };
    unsigned Mod::modNamesNum = sizeof(Mod::modNames)/sizeof(char*);

    bool Mod::_pullElementMatch( BSONElement& toMatch ) const {

        if ( elt.type() != Object ) {
            // if elt isn't an object, then comparison will work
            return toMatch.woCompare( elt , false ) == 0;
        }

        if ( matcherOnPrimitive )
            return matcher->matches( toMatch.wrap( "" ) );

        if ( toMatch.type() != Object ) {
            // looking for an object, so this can't match
            return false;
        }

        // now we have an object on both sides
        return matcher->matches( toMatch.embeddedObject() );
    }

    void Mod::appendIncremented( BSONBuilderBase& builder , const BSONElement& in, ModState& ms ) const {
        BSONType a = in.type();
        BSONType b = elt.type();

        if ( a == NumberDouble || b == NumberDouble ) {
            ms.incType = NumberDouble;
            ms.incdouble = elt.numberDouble() + in.numberDouble();
        }
        else if ( a == NumberLong || b == NumberLong ) {
            ms.incType = NumberLong;
            ms.inclong = elt.numberLong() + in.numberLong();
        }
        else {
            int x = elt.numberInt() + in.numberInt();
            if ( x < 0 && elt.numberInt() > 0 && in.numberInt() > 0 ) {
                // overflow
                ms.incType = NumberLong;
                ms.inclong = elt.numberLong() + in.numberLong();
            }
            else {
                ms.incType = NumberInt;
                ms.incint = elt.numberInt() + in.numberInt();
            }
        }

        ms.appendIncValue( builder , false );
    }

    void appendUnset( BSONBuilderBase& builder ) {
        if ( builder.isArray() ) {
            builder.appendNull();
        }
    }

    void Mod::apply( BSONBuilderBase& builder , BSONElement in , ModState& ms ) const {
        if ( ms.dontApply ) {
            // Pass the original element through unchanged.
            builder << in;
            return;
        }

        switch ( op ) {

        case INC: {
            appendIncremented( builder , in , ms );
            break;
        }

        case SET: {
            _checkForAppending( elt );
            builder.appendAs( elt , shortFieldName );
            break;
        }

        case UNSET: {
            appendUnset( builder );
            break;
        }

        case PUSH: {
            uassert( 10131 ,  "$push can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( builder.subarrayStart( shortFieldName ) );
            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ) {
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            bb.appendAs( elt ,  bb.numStr( n ) );
            bb.done();
            break;
        }

        case ADDTOSET: {
            uassert( 12592 ,  "$addToSet can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( builder.subarrayStart( shortFieldName ) );

            BSONObjIterator i( in.embeddedObject() );
            int n=0;

            if ( isEach() ) {

                BSONElementSet toadd;
                parseEach( toadd );

                while ( i.more() ) {
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;
                    toadd.erase( cur );
                }

                {
                    BSONObjIterator i( getEach() );
                    while ( i.more() ) {
                        BSONElement e = i.next();
                        if ( toadd.count(e) ) {
                            bb.appendAs( e , BSONObjBuilder::numStr( n++ ) );
                            toadd.erase( e );
                        }
                    }
                }

            }
            else {

                bool found = false;

                while ( i.more() ) {
                    BSONElement cur = i.next();
                    bb.append( cur );
                    n++;
                    if ( elt.woCompare( cur , false ) == 0 )
                        found = true;
                }

                if ( ! found )
                    bb.appendAs( elt ,  bb.numStr( n ) );

            }

            bb.done();
            break;
        }



        case PUSH_ALL: {
            uassert( 10132 ,  "$pushAll can only be applied to an array" , in.type() == Array );
            uassert( 10133 ,  "$pushAll has to be passed an array" , elt.type() );

            BSONObjBuilder bb( builder.subarrayStart( shortFieldName ) );

            BSONObjIterator i( in.embeddedObject() );
            int n=0;
            while ( i.more() ) {
                bb.append( i.next() );
                n++;
            }

            ms.pushStartSize = n;

            i = BSONObjIterator( elt.embeddedObject() );
            while ( i.more() ) {
                bb.appendAs( i.next() , bb.numStr( n++ ) );
            }

            bb.done();
            break;
        }

        case PULL:
        case PULL_ALL: {
            uassert( 10134 ,  "$pull/$pullAll can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( builder.subarrayStart( shortFieldName ) );

            //temporarily record the things to pull. only use this set while 'elt' in scope.
            BSONElementSet toPull;
            if ( op == PULL_ALL ) {
                BSONObjIterator j( elt.embeddedObject() );
                while ( j.more() ) {
                    toPull.insert( j.next() );
                }
            }

            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            while ( i.more() ) {
                BSONElement e = i.next();
                bool allowed = true;

                if ( op == PULL ) {
                    allowed = ! _pullElementMatch( e );
                }
                else {
                    allowed = ( toPull.find( e ) == toPull.end() );
                }

                if ( allowed )
                    bb.appendAs( e , bb.numStr( n++ ) );
            }

            bb.done();
            break;
        }

        case POP: {
            uassert( 10135 ,  "$pop can only be applied to an array" , in.type() == Array );
            BSONObjBuilder bb( builder.subarrayStart( shortFieldName ) );

            int n = 0;

            BSONObjIterator i( in.embeddedObject() );
            if ( elt.isNumber() && elt.number() < 0 ) {
                // pop from front
                if ( i.more() ) {
                    i.next();
                    n++;
                }

                while( i.more() ) {
                    bb.appendAs( i.next() , bb.numStr( n - 1 ) );
                    n++;
                }
            }
            else {
                // pop from back
                while( i.more() ) {
                    n++;
                    BSONElement arrI = i.next();
                    if ( i.more() ) {
                        bb.append( arrI );
                    }
                }
            }

            ms.pushStartSize = n;
            verify( ms.pushStartSize == in.embeddedObject().nFields() );
            bb.done();
            break;
        }

        case BIT: {
            uassert( 10136 ,  "$bit needs an object" , elt.type() == Object );
            uassert( 10137 ,  "$bit can only be applied to numbers" , in.isNumber() );
            uassert( 10138 ,  "$bit cannot update a value of type double" , in.type() != NumberDouble );

            int x = in.numberInt();
            long long y = in.numberLong();

            BSONObjIterator it( elt.embeddedObject() );
            while ( it.more() ) {
                BSONElement e = it.next();
                uassert( 10139 ,  "$bit field must be number" , e.isNumber() );
                if ( str::equals(e.fieldName(), "and") ) {
                    switch( in.type() ) {
                    case NumberInt: x = x&e.numberInt(); break;
                    case NumberLong: y = y&e.numberLong(); break;
                    default: verify( 0 );
                    }
                }
                else if ( str::equals(e.fieldName(), "or") ) {
                    switch( in.type() ) {
                    case NumberInt: x = x|e.numberInt(); break;
                    case NumberLong: y = y|e.numberLong(); break;
                    default: verify( 0 );
                    }
                }
                else {
                    uasserted(9016, str::stream() << "unknown $bit operation: " << e.fieldName());
                }
            }

            switch( in.type() ) {
            case NumberInt: builder.append( shortFieldName , x ); break;
            case NumberLong: builder.append( shortFieldName , y ); break;
            default: verify( 0 );
            }

            break;
        }

        case RENAME_FROM: {
            break;
        }

        case RENAME_TO: {
            ms.handleRename( builder, shortFieldName );
            break;
        }

        default:
            uasserted( 9017 , str::stream() << "Mod::apply can't handle type: " << op );
        }
    }

    // -1 inside a non-object (non-object could be array)
    // 0 missing
    // 1 found
    int validRenamePath( BSONObj obj, const char* path ) {
        while( const char* p = strchr( path, '.' ) ) {
            string left( path, p - path );
            BSONElement e = obj.getField( left );
            if ( e.eoo() ) {
                return 0;
            }
            if ( e.type() != Object ) {
                return -1;
            }
            obj = e.embeddedObject();
            path = p + 1;
        }
        return !obj.getField( path ).eoo();
    }

    auto_ptr<ModSetState> ModSet::prepare(const BSONObj& obj) const {
        DEBUGUPDATE( "\t start prepare" );
        auto_ptr<ModSetState> mss( new ModSetState( obj ) );


        // Perform this check first, so that we don't leave a partially modified object on uassert.
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            DEBUGUPDATE( "\t\t prepare : " << i->first );
            mss->_mods[i->first].reset( new ModState() );
            ModState& ms = *mss->_mods[i->first];

            const Mod& m = i->second;
            BSONElement e = obj.getFieldDotted(m.fieldName);

            ms.m = &m;
            ms.old = e;

            if ( m.op == Mod::RENAME_FROM ) {
                int source = validRenamePath( obj, m.fieldName );
                uassert( 13489, "$rename source field invalid", source != -1 );
                if ( source != 1 ) {
                    ms.dontApply = true;
                }
                continue;
            }

            if ( m.op == Mod::RENAME_TO ) {
                int source = validRenamePath( obj, m.renameFrom() );
                if ( source == 1 ) {
                    int target = validRenamePath( obj, m.fieldName );
                    uassert( 13490, "$rename target field invalid", target != -1 );
                    ms.newVal = obj.getFieldDotted( m.renameFrom() );
                    mss->amIInPlacePossible( false );
                }
                else {
                    ms.dontApply = true;
                }
                continue;
            }

            if ( e.eoo() ) {
                mss->amIInPlacePossible( m.op == Mod::UNSET );
                continue;
            }

            switch( m.op ) {
            case Mod::INC:
                uassert( 10140 ,  "Cannot apply $inc modifier to non-number", e.isNumber() || e.eoo() );
                if ( mss->amIInPlacePossible( e.isNumber() ) ) {
                    // check more typing info here
                    if ( m.elt.type() != e.type() ) {
                        // if i'm incrementing with a double, then the storage has to be a double
                        mss->amIInPlacePossible( m.elt.type() != NumberDouble );
                    }

                    // check for overflow
                    if ( e.type() == NumberInt && e.numberLong() + m.elt.numberLong() > numeric_limits<int>::max() ) {
                        mss->amIInPlacePossible( false );
                    }
                }
                break;

            case Mod::SET:
                mss->amIInPlacePossible( m.elt.type() == e.type() &&
                                         m.elt.valuesize() == e.valuesize() );
                break;

            case Mod::PUSH:
            case Mod::PUSH_ALL:
                uassert( 10141,
                         "Cannot apply $push/$pushAll modifier to non-array",
                         e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( false );
                break;

            case Mod::PULL:
            case Mod::PULL_ALL: {
                uassert( 10142,
                         "Cannot apply $pull/$pullAll modifier to non-array",
                         e.type() == Array || e.eoo() );

                //temporarily record the things to pull. only use this set while 'm.elt' in scope.
                BSONElementSet toPull;
                if ( m.op == Mod::PULL_ALL ) {
                    BSONObjIterator j( m.elt.embeddedObject() );
                    while ( j.more() ) {
                        toPull.insert( j.next() );
                    }
                }

                BSONObjIterator i( e.embeddedObject() );
                while( mss->_inPlacePossible && i.more() ) {
                    BSONElement arrI = i.next();
                    if ( m.op == Mod::PULL ) {
                        mss->amIInPlacePossible( ! m._pullElementMatch( arrI ) );
                    }
                    else if ( m.op == Mod::PULL_ALL ) {
                        mss->amIInPlacePossible( toPull.find( arrI ) == toPull.end() );
                    }
                }
                break;
            }

            case Mod::POP: {
                uassert( 10143,
                         "Cannot apply $pop modifier to non-array",
                         e.type() == Array || e.eoo() );
                mss->amIInPlacePossible( e.embeddedObject().isEmpty() );
                break;
            }

            case Mod::ADDTOSET: {
                uassert( 12591,
                         "Cannot apply $addToSet modifier to non-array",
                         e.type() == Array || e.eoo() );

                BSONObjIterator i( e.embeddedObject() );
                if ( m.isEach() ) {
                    BSONElementSet toadd;
                    m.parseEach( toadd );
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        toadd.erase( arrI );
                    }
                    mss->amIInPlacePossible( toadd.size() == 0 );
                }
                else {
                    bool found = false;
                    while( i.more() ) {
                        BSONElement arrI = i.next();
                        if ( arrI.woCompare( m.elt , false ) == 0 ) {
                            found = true;
                            break;
                        }
                    }
                    mss->amIInPlacePossible( found );
                }
                break;
            }

            default:
                // mods we don't know about shouldn't be done in place
                mss->amIInPlacePossible( false );
            }
        }

        DEBUGUPDATE( "\t mss\n" << mss->toString() << "\t--" );

        return mss;
    }

    void ModState::appendForOpLog( BSONObjBuilder& b ) const {
        if ( dontApply ) {
            return;
        }

        if ( incType ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog inc fieldname: " << m->fieldName
                         << " short:" << m->shortFieldName );
            BSONObjBuilder bb( b.subobjStart( "$set" ) );
            appendIncValue( bb , true );
            bb.done();
            return;
        }

        if ( m->op == Mod::RENAME_FROM ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_FROM fieldName:" << m->fieldName );
            BSONObjBuilder bb( b.subobjStart( "$unset" ) );
            bb.append( m->fieldName, 1 );
            bb.done();
            return;
        }

        if ( m->op == Mod::RENAME_TO ) {
            DEBUGUPDATE( "\t\t\t\t\t appendForOpLog RENAME_TO fieldName:" << m->fieldName );
            BSONObjBuilder bb( b.subobjStart( "$set" ) );
            bb.appendAs( newVal, m->fieldName );
            return;
        }

        const char* name = fixedOpName ? fixedOpName : Mod::modNames[op()];

        DEBUGUPDATE( "\t\t\t\t\t appendForOpLog name:" << name << " fixed: " << fixed
                     << " fn: " << m->fieldName );

        BSONObjBuilder bb( b.subobjStart( name ) );
        if ( fixed ) {
            bb.appendAs( *fixed , m->fieldName );
        }
        else {
            bb.appendAs( m->elt , m->fieldName );
        }
        bb.done();
    }

    string ModState::toString() const {
        stringstream ss;
        if ( fixedOpName )
            ss << " fixedOpName: " << fixedOpName;
        if ( fixed )
            ss << " fixed: " << fixed;
        return ss.str();
    }

    void ModState::handleRename( BSONBuilderBase& newObjBuilder, const char* shortFieldName ) {
        newObjBuilder.appendAs( newVal , shortFieldName );
        BSONObjBuilder b;
        b.appendAs( newVal, shortFieldName );
        verify( _objData.isEmpty() );
        _objData = b.obj();
        newVal = _objData.firstElement();
    }

    void ModSetState::applyModsInPlace( bool isOnDisk ) {
        // TODO i think this assert means that we can get rid of the isOnDisk param
        //      and just use isOwned as the determination
        DEV verify( isOnDisk == ! _obj.isOwned() );

        for ( ModStateHolder::iterator i = _mods.begin(); i != _mods.end(); ++i ) {
            ModState& m = *i->second;

            if ( m.dontApply ) {
                continue;
            }

            switch ( m.m->op ) {
            case Mod::UNSET:
            case Mod::ADDTOSET:
            case Mod::RENAME_FROM:
            case Mod::RENAME_TO:
                // this should have been handled by prepare
                break;
            case Mod::PULL:
            case Mod::PULL_ALL:
                // this should have been handled by prepare
                break;
            case Mod::POP:
                verify( m.old.eoo() || ( m.old.isABSONObj() && m.old.Obj().isEmpty() ) );
                break;
                // [dm] the BSONElementManipulator statements below are for replication (correct?)
            case Mod::INC:
                if ( isOnDisk )
                    m.m->IncrementMe( m.old );
                else
                    m.m->incrementMe( m.old );
                m.fixedOpName = "$set";
                m.fixed = &(m.old);
                break;
            case Mod::SET:
                if ( isOnDisk )
                    BSONElementManipulator( m.old ).ReplaceTypeAndValue( m.m->elt );
                else
                    BSONElementManipulator( m.old ).replaceTypeAndValue( m.m->elt );
                break;
            default:
                uassert( 13478 ,  "can't apply mod in place - shouldn't have gotten here" , 0 );
            }
        }
    }

    void ModSetState::_appendNewFromMods( const string& root,
                                          ModState& modState,
                                          BSONBuilderBase& builder,
                                          set<string>& onedownseen ) {
        const char*  temp = modState.fieldName();
        temp += root.size();
        const char* dot = strchr( temp , '.' );
        if ( dot ) {
            string nr( modState.fieldName() , 0 , 1 + ( dot - modState.fieldName() ) );
            string nf( temp , 0 , dot - temp );
            if ( onedownseen.count( nf ) )
                return;
            onedownseen.insert( nf );
            BSONObjBuilder bb ( builder.subobjStart( nf ) );
            // Always insert an object, even if the field name is numeric.
            createNewObjFromMods( nr , bb , BSONObj() );
            bb.done();
        }
        else {
            appendNewFromMod( modState , builder );
        }
    }

    bool ModSetState::duplicateFieldName( const BSONElement& a, const BSONElement& b ) {
        return
        !a.eoo() &&
        !b.eoo() &&
        ( a.rawdata() != b.rawdata() ) &&
        str::equals( a.fieldName(), b.fieldName() );
    }

    ModSetState::ModStateRange ModSetState::modsForRoot( const string& root ) {
        ModStateHolder::iterator mstart = _mods.lower_bound( root );
        StringBuilder buf;
        buf << root << (char)255;
        ModStateHolder::iterator mend = _mods.lower_bound( buf.str() );
        return make_pair( mstart, mend );
    }

    void ModSetState::createNewObjFromMods( const string& root,
                                            BSONObjBuilder& builder,
                                            const BSONObj& obj ) {
        BSONObjIteratorSorted es( obj );
        createNewFromMods( root, builder, es, modsForRoot( root ), LexNumCmp( true ) );
    }

    void ModSetState::createNewArrayFromMods( const string& root,
                                              BSONArrayBuilder& builder,
                                              const BSONArray& arr ) {
        BSONArrayIteratorSorted es( arr );
        ModStateRange objectOrderedRange = modsForRoot( root );
        ModStateHolder arrayOrderedMods( LexNumCmp( false ) );
        arrayOrderedMods.insert( objectOrderedRange.first, objectOrderedRange.second );
        ModStateRange arrayOrderedRange( arrayOrderedMods.begin(), arrayOrderedMods.end() );
        createNewFromMods( root, builder, es, arrayOrderedRange, LexNumCmp( false ) );
    }

    void ModSetState::createNewFromMods( const string& root,
                                         BSONBuilderBase& builder,
                                         BSONIteratorSorted& es,
                                         const ModStateRange& modRange,
                                         const LexNumCmp& lexNumCmp ) {

        DEBUGUPDATE( "\t\t createNewFromMods root: " << root );
        ModStateHolder::iterator m = modRange.first;
        const ModStateHolder::const_iterator mend = modRange.second;
        BSONElement e = es.next();

        set<string> onedownseen;
        BSONElement prevE;
        while ( !e.eoo() && m != mend ) {

            if ( duplicateFieldName( prevE, e ) ) {
                // Just copy through an element with a duplicate field name.
                builder.append( e );
                prevE = e;
                e = es.next();
                continue;
            }
            prevE = e;

            string field = root + e.fieldName();
            FieldCompareResult cmp = compareDottedFieldNames( m->second->m->fieldName , field ,
                                                             lexNumCmp );

            DEBUGUPDATE( "\t\t\t field:" << field << "\t mod:"
                         << m->second->m->fieldName << "\t cmp:" << cmp
                         << "\t short: " << e.fieldName() );

            switch ( cmp ) {

            case LEFT_SUBFIELD: { // Mod is embedded under this element
                uassert( 10145,
                         str::stream() << "LEFT_SUBFIELD only supports Object: " << field
                         << " not: " << e.type() , e.type() == Object || e.type() == Array );
                if ( onedownseen.count( e.fieldName() ) == 0 ) {
                    onedownseen.insert( e.fieldName() );
                    if ( e.type() == Object ) {
                        BSONObjBuilder bb( builder.subobjStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewObjFromMods( nr.str() , bb , e.Obj() );
                        bb.done();
                    }
                    else {
                        BSONArrayBuilder ba( builder.subarrayStart( e.fieldName() ) );
                        stringstream nr; nr << root << e.fieldName() << ".";
                        createNewArrayFromMods( nr.str() , ba , BSONArray( e.embeddedObject() ) );
                        ba.done();
                    }
                    // inc both as we handled both
                    e = es.next();
                    m++;
                }
                else {
                    massert( 16069 , "ModSet::createNewFromMods - "
                            "SERVER-4777 unhandled duplicate field" , 0 );
                }
                continue;
            }
            case LEFT_BEFORE: // Mod on a field that doesn't exist
                DEBUGUPDATE( "\t\t\t\t creating new field for: " << m->second->m->fieldName );
                _appendNewFromMods( root , *m->second , builder , onedownseen );
                m++;
                continue;
            case SAME:
                DEBUGUPDATE( "\t\t\t\t applying mod on: " << m->second->m->fieldName );
                m->second->apply( builder , e );
                e = es.next();
                m++;
                continue;
            case RIGHT_BEFORE: // field that doesn't have a MOD
                DEBUGUPDATE( "\t\t\t\t just copying" );
                builder.append( e ); // if array, ignore field name
                e = es.next();
                continue;
            case RIGHT_SUBFIELD:
                massert( 10399 ,  "ModSet::createNewFromMods - RIGHT_SUBFIELD should be impossible" , 0 );
                break;
            default:
                massert( 10400 ,  "unhandled case" , 0 );
            }
        }

        // finished looping the mods, just adding the rest of the elements
        while ( !e.eoo() ) {
            DEBUGUPDATE( "\t\t\t copying: " << e.fieldName() );
            builder.append( e );  // if array, ignore field name
            e = es.next();
        }

        // do mods that don't have fields already
        for ( ; m != mend; m++ ) {
            DEBUGUPDATE( "\t\t\t\t appending from mod at end: " << m->second->m->fieldName );
            _appendNewFromMods( root , *m->second , builder , onedownseen );
        }
    }

    BSONObj ModSetState::createNewFromMods() {
        BSONObjBuilder b( (int)(_obj.objsize() * 1.1) );
        createNewObjFromMods( "" , b , _obj );
        return _newFromMods = b.obj();
    }

    string ModSetState::toString() const {
        stringstream ss;
        for ( ModStateHolder::const_iterator i=_mods.begin(); i!=_mods.end(); ++i ) {
            ss << "\t\t" << i->first << "\t" << i->second->toString() << "\n";
        }
        return ss.str();
    }

    BSONObj ModSet::createNewFromQuery( const BSONObj& query ) {
        BSONObj newObj;

        {
            BSONObjBuilder bb;
            EmbeddedBuilder eb( &bb );
            BSONObjIteratorSorted i( query );
            while ( i.more() ) {
                BSONElement e = i.next();
                if ( e.fieldName()[0] == '$' ) // for $atomic and anything else we add
                    continue;

                if ( e.type() == Object && e.embeddedObject().firstElementFieldName()[0] == '$' ) {
                    // we have something like { x : { $gt : 5 } }
                    // this can be a query piece
                    // or can be a dbref or something
                    
                    int op = e.embeddedObject().firstElement().getGtLtOp( -1 );
                    if ( op >= 0 ) {
                        // this means this is a $gt type filter, so don't make part of the new object
                        continue;
                    }
                }

                eb.appendAs( e , e.fieldName() );
            }
            eb.done();
            newObj = bb.obj();
        }

        auto_ptr<ModSetState> mss = prepare( newObj );

        if ( mss->canApplyInPlace() )
            mss->applyModsInPlace( false );
        else
            newObj = mss->createNewFromMods();

        return newObj;
    }

    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       { $push: { a:55 } }
       { $pushAll: { a:[77,88] } }
       { $pull: { a:66 } }
       { $pullAll : { a:[99,1010] } }
       NOTE: MODIFIES source from object!
    */
    ModSet::ModSet(
        const BSONObj& from ,
        const set<string>& idxKeys,
        const set<string>* backgroundKeys)
        : _isIndexed(0) , _hasDynamicArray( false ) {

        BSONObjIterator it(from);

        while ( it.more() ) {
            BSONElement e = it.next();
            const char* fn = e.fieldName();

            uassert( 10147 ,  "Invalid modifier specified: " + string( fn ), e.type() == Object );
            BSONObj j = e.embeddedObject();
            DEBUGUPDATE( "\t" << j );

            BSONObjIterator jt(j);
            Mod::Op op = opFromStr( fn );

            while ( jt.more() ) {
                BSONElement f = jt.next(); // x:44

                const char* fieldName = f.fieldName();

                // Allow remove of invalid field name in case it was inserted before this check
                // was added (~ version 2.1).
                uassert( 15896,
                         "Modified field name may not start with $",
                         fieldName[0] != '$' || op == Mod::UNSET );
                uassert( 10148,
                         "Mod on _id not allowed",
                         strcmp( fieldName, "_id" ) != 0 );
                uassert( 10149,
                         "Invalid mod field name, may not end in a period",
                         fieldName[ strlen( fieldName ) - 1 ] != '.' );
                uassert( 10150,
                         "Field name duplication not allowed with modifiers",
                         ! haveModForField( fieldName ) );
                uassert( 10151,
                         "have conflicting mods in update",
                         ! haveConflictingMod( fieldName ) );
                uassert( 10152,
                         "Modifier $inc allowed for numbers only",
                         f.isNumber() || op != Mod::INC );
                uassert( 10153,
                         "Modifier $pushAll/pullAll allowed for arrays only",
                         f.type() == Array || ( op != Mod::PUSH_ALL && op != Mod::PULL_ALL ) );

                if ( op == Mod::RENAME_TO ) {
                    uassert( 13494, "$rename target must be a string", f.type() == String );
                    const char* target = f.valuestr();
                    uassert( 13495,
                             "$rename source must differ from target",
                             strcmp( fieldName, target ) != 0 );
                    uassert( 13496,
                             "invalid mod field name, source may not be empty",
                             fieldName[0] );
                    uassert( 13479,
                             "invalid mod field name, target may not be empty",
                             target[0] );
                    uassert( 13480,
                             "invalid mod field name, source may not begin or end in period",
                             fieldName[0] != '.' && fieldName[ strlen( fieldName ) - 1 ] != '.' );
                    uassert( 13481,
                             "invalid mod field name, target may not begin or end in period",
                             target[0] != '.' && target[ strlen( target ) - 1 ] != '.' );
                    uassert( 13482,
                             "$rename affecting _id not allowed",
                             !( fieldName[0] == '_' && fieldName[1] == 'i' && fieldName[2] == 'd'
                                && ( !fieldName[3] || fieldName[3] == '.' ) ) );
                    uassert( 13483,
                             "$rename affecting _id not allowed",
                             !( target[0] == '_' && target[1] == 'i' && target[2] == 'd'
                                && ( !target[3] || target[3] == '.' ) ) );
                    uassert( 13484,
                             "field name duplication not allowed with $rename target",
                             !haveModForField( target ) );
                    uassert( 13485,
                             "conflicting mods not allowed with $rename target",
                             !haveConflictingMod( target ) );
                    uassert( 13486,
                             "$rename target may not be a parent of source",
                             !( strncmp( fieldName, target, strlen( target ) ) == 0
                                && fieldName[ strlen( target ) ] == '.' ) );
                    uassert( 13487,
                             "$rename source may not be dynamic array",
                             strstr( fieldName , ".$" ) == 0 );
                    uassert( 13488,
                             "$rename target may not be dynamic array",
                             strstr( target , ".$" ) == 0 );

                    Mod from;
                    from.init( Mod::RENAME_FROM, f );
                    from.setFieldName( fieldName );
                    updateIsIndexed( from, idxKeys, backgroundKeys );
                    _mods[ from.fieldName ] = from;

                    Mod to;
                    to.init( Mod::RENAME_TO, f );
                    to.setFieldName( target );
                    updateIsIndexed( to, idxKeys, backgroundKeys );
                    _mods[ to.fieldName ] = to;

                    DEBUGUPDATE( "\t\t " << fieldName << "\t" << from.fieldName << "\t" << to.fieldName );
                    continue;
                }

                _hasDynamicArray = _hasDynamicArray || strstr( fieldName , ".$" ) > 0;

                Mod m;
                m.init( op , f );
                m.setFieldName( f.fieldName() );
                updateIsIndexed( m, idxKeys, backgroundKeys );
                _mods[m.fieldName] = m;

                DEBUGUPDATE( "\t\t " << fieldName << "\t" << m.fieldName << "\t" << _hasDynamicArray );
            }
        }

    }

    ModSet* ModSet::fixDynamicArray( const string& elemMatchKey ) const {
        ModSet* n = new ModSet();
        n->_isIndexed = _isIndexed;
        n->_hasDynamicArray = _hasDynamicArray;
        for ( ModHolder::const_iterator i=_mods.begin(); i!=_mods.end(); i++ ) {
            string s = i->first;
            size_t idx = s.find( ".$" );
            if ( idx == string::npos ) {
                n->_mods[s] = i->second;
                continue;
            }
            StringBuilder buf;
            buf << s.substr(0,idx+1) << elemMatchKey << s.substr(idx+2);
            string fixed = buf.str();
            DEBUGUPDATE( "fixed dynamic: " << s << " -->> " << fixed );
            n->_mods[fixed] = i->second;
            ModHolder::iterator temp = n->_mods.find( fixed );
            temp->second.setFieldName( temp->first.c_str() );
        }
        return n;
    }

    void ModSet::updateIsIndexed( const set<string>& idxKeys, const set<string>* backgroundKeys ) {
        for ( ModHolder::const_iterator i = _mods.begin(); i != _mods.end(); ++i )
            updateIsIndexed( i->second, idxKeys , backgroundKeys );
    }

} // namespace mongo